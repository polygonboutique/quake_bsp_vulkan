#include "q3bsp/Q3BspMap.hpp"
#include "q3bsp/Q3BspPatch.hpp"
#include "renderer/TextureManager.hpp"
#include "renderer/vulkan/CmdBuffer.hpp"
#include "renderer/vulkan/Pipeline.hpp"
#include "Math.hpp"
#include "ThreadProcessor.hpp"
#include "Utils.hpp"
#include <algorithm>
#include <sstream>

extern RenderContext   g_renderContext;
extern ThreadProcessor g_threadProcessor;
const int   Q3BspMap::s_tesselationLevel = 10;   // level of curved surface tesselation
const float Q3BspMap::s_worldScale       = 64.f; // scale down factor for the map

Q3BspMap::~Q3BspMap()
{
    delete[] entities.ents;
    delete[] visData.vecs;

    for (auto &it : m_patches)
        delete it;

    // release all allocated Vulkan resources
    vk::destroyPipeline(g_renderContext.Device(), m_facesPipeline);
    vk::destroyPipeline(g_renderContext.Device(), m_patchPipeline);

    vk::freeBuffer(g_renderContext.Device(), m_faceVertexBuffer);
    vk::freeBuffer(g_renderContext.Device(), m_faceIndexBuffer);
    vk::freeBuffer(g_renderContext.Device(), m_patchVertexBuffer);
    vk::freeBuffer(g_renderContext.Device(), m_patchIndexBuffer);

    for (size_t i = 0; i < lightMaps.size(); ++i)
    {
        vk::releaseTexture(g_renderContext.Device(), m_lightmapTextures[i]);
    }
    delete[] m_lightmapTextures;

    vk::freeBuffer(g_renderContext.Device(), m_renderBuffers.uniformBuffer);
    vk::releaseTexture(g_renderContext.Device(), m_whiteTex);
    vkDestroyDescriptorPool(g_renderContext.Device().logical, m_descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(g_renderContext.Device().logical, m_dsLayout, nullptr);

    for (unsigned int i = 0; i < g_threadProcessor.NumThreads(); ++i)
    {
        vkFreeCommandBuffers(g_renderContext.Device().logical, m_commandPools[i], 1, &m_commandBuffers[0][i]);
        vkFreeCommandBuffers(g_renderContext.Device().logical, m_commandPools[i], 1, &m_commandBuffers[1][i]);
        vkDestroyCommandPool(g_renderContext.Device().logical, m_commandPools[i], nullptr);
    }
}

void Q3BspMap::Init()
{
    // setup render buffers - take multithreading into account (if enabled)
    unsigned int threadCnt = g_threadProcessor.NumThreads();
    m_facesPerThread = (int)leafFaces.size() / threadCnt;

    m_visibleFacesPerThread.resize(threadCnt);
    m_visiblePatchesPerThread.resize(threadCnt);
    m_commandPools.resize(threadCnt);
    for (unsigned int i = 0; i < threadCnt; ++i)
    {
        VK_VERIFY(vk::createCommandPool(g_renderContext.Device(), g_renderContext.Device().graphicsFamilyIndex, &m_commandPools[i]));
        m_commandBuffers[0].push_back(vk::createCommandBuffer(g_renderContext.Device(), m_commandPools[i], VK_COMMAND_BUFFER_LEVEL_SECONDARY));
        m_commandBuffers[1].push_back(vk::createCommandBuffer(g_renderContext.Device(), m_commandPools[i], VK_COMMAND_BUFFER_LEVEL_SECONDARY));
    }

    // if there are no faces, this means a problem or a missing BSP - abort
    if (faces.empty())
        return;

    // regular faces are simple triangle lists, patches are drawn as triangle strips, so we need extra pipeline
    m_facesPipeline.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    m_patchPipeline.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    m_facesPipeline.cache = g_renderContext.PipelineCache();
    m_patchPipeline.cache = g_renderContext.PipelineCache();
    // use pipeline derivatives to create patch pipeline using faces pipeline as base
    m_facesPipeline.flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
    m_patchPipeline.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;

    // stub missing texture used if original Quake assets are missing
    m_missingTex = TextureManager::GetInstance()->LoadTexture("res/missing.png");

    // load textures
    LoadTextures();

    // load lightmaps
    LoadLightmaps();

    // create renderable faces and patches
    m_renderLeaves.reserve(leaves.size());

    for (const auto &l : leaves)
    {
        m_renderLeaves.push_back(Q3LeafRenderable());
        m_renderLeaves.back().visCluster = l.cluster;
        m_renderLeaves.back().firstFace  = l.leafFace;
        m_renderLeaves.back().numFaces   = l.n_leafFaces;

        // create a bounding box
        m_renderLeaves.back().boundingBoxVertices[0] = Math::Vector3f((float)l.mins.x, (float)l.mins.y, (float)l.mins.z);
        m_renderLeaves.back().boundingBoxVertices[1] = Math::Vector3f((float)l.mins.x, (float)l.mins.y, (float)l.maxs.z);
        m_renderLeaves.back().boundingBoxVertices[2] = Math::Vector3f((float)l.mins.x, (float)l.maxs.y, (float)l.mins.z);
        m_renderLeaves.back().boundingBoxVertices[3] = Math::Vector3f((float)l.mins.x, (float)l.maxs.y, (float)l.maxs.z);
        m_renderLeaves.back().boundingBoxVertices[4] = Math::Vector3f((float)l.maxs.x, (float)l.mins.y, (float)l.mins.z);
        m_renderLeaves.back().boundingBoxVertices[5] = Math::Vector3f((float)l.maxs.x, (float)l.mins.y, (float)l.maxs.z);
        m_renderLeaves.back().boundingBoxVertices[6] = Math::Vector3f((float)l.maxs.x, (float)l.maxs.y, (float)l.mins.z);
        m_renderLeaves.back().boundingBoxVertices[7] = Math::Vector3f((float)l.maxs.x, (float)l.maxs.y, (float)l.maxs.z);

        for (int i = 0; i < 8; ++i)
        {
            m_renderLeaves.back().boundingBoxVertices[i].m_x /= Q3BspMap::s_worldScale;
            m_renderLeaves.back().boundingBoxVertices[i].m_y /= Q3BspMap::s_worldScale;
            m_renderLeaves.back().boundingBoxVertices[i].m_z /= Q3BspMap::s_worldScale;
        }
    }

    m_renderFaces.reserve(faces.size());

    // create a common descriptor set layout and vertex buffer info
    m_vbInfo.bindingDescriptions.push_back(vk::getBindingDescription(sizeof(Q3BspVertexLump)));
    m_vbInfo.attributeDescriptions.push_back(vk::getAttributeDescription(inVertex, VK_FORMAT_R32G32B32_SFLOAT, 0));
    m_vbInfo.attributeDescriptions.push_back(vk::getAttributeDescription(inTexCoord, VK_FORMAT_R32G32_SFLOAT, sizeof(vec3f)));
    m_vbInfo.attributeDescriptions.push_back(vk::getAttributeDescription(inTexCoordLightmap, VK_FORMAT_R32G32_SFLOAT, sizeof(vec3f) + sizeof(vec2f)));
    CreateDescriptorSetLayout();

    // create descriptor pool shared between all visible faces and patches (one descriptor per visible face)
    CreateDescriptorPool((uint32_t)faces.size());

    // single shared uniform buffer
    VK_VERIFY(vk::createUniformBuffer(g_renderContext.Device(), sizeof(UniformBufferObject), &m_renderBuffers.uniformBuffer));

    int faceArrayIdx  = 0;
    int patchArrayIdx = 0;

    int numFaceVerts = 0;
    int numFaceIndexes = 0;
    int numPatchVerts = 0;
    int numPatchIndexes = 0;

    // data agregators for vertex and index buffer creation
    std::vector<Q3BspFaceLump*> faceData;
    std::vector<Q3BspBiquadPatch*> patchData;
    std::vector<int> patchIndexData;

    for (auto &f : faces)
    {
        m_renderFaces.push_back(Q3FaceRenderable());

        //is it a patch?
        if (f.type == FaceTypePatch)
        {
            m_renderFaces.back().index = patchArrayIdx;
            CreatePatch(f);

            // generate Vulkan descriptors for current patch
            CreateDescriptorsForPatch(patchArrayIdx, numPatchVerts, numPatchIndexes, patchData, patchIndexData);
            ++patchArrayIdx;
        }
        else
        {
            m_renderFaces.back().index = faceArrayIdx;

            // generate Vulkan descriptors for current face
            CreateDescriptorsForFace(f, faceArrayIdx, numFaceVerts, numFaceIndexes);
            faceData.push_back(&f);
            numFaceVerts += f.n_vertexes;
            numFaceIndexes += f.n_meshverts;
        }

        ++faceArrayIdx;
        m_renderFaces.back().type = f.type;
    }

    // create single, large index and vertex buffers for faces and patches
    // this is several magnitudes faster than separate buffers for each face/patch
    CreatePatchBuffers(patchData, numPatchVerts, numPatchIndexes);
    CreateFaceBuffers(faceData, numFaceVerts, numFaceIndexes);

    m_mapStats.totalVertices = (int)vertices.size();
    m_mapStats.totalFaces    = (int)faces.size();
    m_mapStats.totalPatches  = patchArrayIdx;

    RebuildPipeline();

    // set the scale-down uniform
    m_pc.worldScaleFactor = 1.f / Q3BspMap::s_worldScale;
}

void Q3BspMap::OnRender()
{
    // no faces at all in this BSP? something's not right, abort
    if (faces.empty())
        return;

    unsigned int threadCnt = g_threadProcessor.NumThreads();

    // update uniform buffers
    m_ubo.ModelViewProjectionMatrix = g_renderContext.ModelViewProjectionMatrix;
    m_frustum.UpdatePlanes();

    void *data;
    vmaMapMemory(g_renderContext.Device().allocator, m_renderBuffers.uniformBuffer.allocation, &data);
    memcpy(data, &m_ubo, sizeof(m_ubo));
    vmaUnmapMemory(g_renderContext.Device().allocator, m_renderBuffers.uniformBuffer.allocation);

    VkCommandBufferInheritanceInfo inheritanceInfo = {};
    inheritanceInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inheritanceInfo.renderPass = g_renderContext.ActiveRenderPass().renderPass;
    inheritanceInfo.framebuffer = g_renderContext.ActiveFramebuffer();

    // record new set of command buffers including only visible faces and patches
    std::vector<VkCommandBuffer> buffersToRender;
    if (threadCnt > 1)
    {
        for (unsigned int i = 0; i < threadCnt; ++i)
        {
            g_threadProcessor.AddTask(i, [=] { Draw(i, inheritanceInfo); });
        }

        // fill all threaded secondary command buffers before final submission to primary command buffer
        g_threadProcessor.Wait();
    }
    else
    {
        Draw(0, inheritanceInfo);
    }

    // queue for rendering only non-empty command buffers
    for (unsigned int i = 0; i < threadCnt; ++i)
    {
        if (!m_visibleFacesPerThread[i].empty() || !m_visiblePatchesPerThread[i].empty())
        {
            buffersToRender.push_back(m_commandBuffers[g_renderContext.ActiveFrame()][i]);
        }
    }

    if (!buffersToRender.empty())
        vkCmdExecuteCommands(g_renderContext.ActiveCmdBuffer(), (uint32_t)buffersToRender.size(), buffersToRender.data());
}

void Q3BspMap::OnUpdate(const Math::Vector3f &cameraPosition)
{
    //calculate the camera leaf
    int cameraLeaf = FindCameraLeaf(cameraPosition * Q3BspMap::s_worldScale);

    if (g_threadProcessor.NumThreads() > 1)
    {
        for (unsigned int i = 0; i < g_threadProcessor.NumThreads(); ++i)
        {
            g_threadProcessor.AddTask(i, [=] { CalculateVisibleFaces(i, cameraLeaf); });
        }
    }
    else
    {
        CalculateVisibleFaces(0, cameraLeaf);
    }
}

void Q3BspMap::RebuildPipeline()
{
    vk::destroyPipeline(g_renderContext.Device(), m_facesPipeline);
    vk::destroyPipeline(g_renderContext.Device(), m_patchPipeline);

    const char *shaders[] = { "res/Basic_vert.spv", "res/Basic_frag.spv" };
    m_facesPipeline.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    m_facesPipeline.pushConstantRange.size = sizeof(BspPushConstants);
    m_facesPipeline.pushConstantRangeCount = 1;
    VK_VERIFY(vk::createPipeline(g_renderContext.Device(), g_renderContext.SwapChain(), g_renderContext.ActiveRenderPass(), m_dsLayout, &m_vbInfo, &m_facesPipeline, shaders));
    m_patchPipeline.pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    m_patchPipeline.pushConstantRange.size = sizeof(BspPushConstants);
    m_patchPipeline.basePipelineHandle = m_facesPipeline.pipeline;
    m_patchPipeline.pushConstantRangeCount = 1;
    VK_VERIFY(vk::createPipeline(g_renderContext.Device(), g_renderContext.SwapChain(), g_renderContext.ActiveRenderPass(), m_dsLayout, &m_vbInfo, &m_patchPipeline, shaders));
}

std::string Q3BspMap::ThreadAndBspStats()
{
    std::string threadStats;

    m_mapStats.visibleFaces = 0;
    m_mapStats.visiblePatches = 0;
    for (unsigned int i = 0; i < g_threadProcessor.NumThreads(); ++i)
    {
        // safe to perform a read from visibility sets without a mutex, since by this point thread processor had waited for all threads to finish, so no writes will occur
        m_mapStats.visibleFaces += (int)m_visibleFacesPerThread[i].size();
        m_mapStats.visiblePatches += (int)m_visiblePatchesPerThread[i].size();
        threadStats += "[#" + std::to_string(i) + ": " + std::to_string(m_visibleFacesPerThread[i].size()) + ", " + std::to_string(m_visiblePatchesPerThread[i].size()) + "]";
    }

    return threadStats;
}

// determine if a bsp cluster is visible from a given camera cluster
bool Q3BspMap::ClusterVisible(int cameraCluster, int testCluster) const
{
    if (!visData.vecs || (cameraCluster < 0)) {
        return true;
    }

    int idx = (cameraCluster * visData.sz_vecs) + (testCluster >> 3);

    return (visData.vecs[idx] & (1 << (testCluster & 7))) != 0;
}

// determine which bsp leaf camera resides in
int Q3BspMap::FindCameraLeaf(const Math::Vector3f &cameraPosition) const
{
    int leafIndex = 0;

    while (leafIndex >= 0)
    {
        // children.x - front node; children.y - back node
        if (PointPlanePos(planes[nodes[leafIndex].plane].normal.x,
                          planes[nodes[leafIndex].plane].normal.y,
                          planes[nodes[leafIndex].plane].normal.z,
                          planes[nodes[leafIndex].plane].dist, 
                          cameraPosition) == Math::PointInFrontOfPlane)
        {
            leafIndex = nodes[leafIndex].children.x;
        }
        else
        {
            leafIndex = nodes[leafIndex].children.y;
        }
    }

    return ~leafIndex;
}


//Calculate which faces to draw given a camera position & view frustum
void Q3BspMap::CalculateVisibleFaces(int threadIndex, int cameraLeaf)
{
    m_visibleFacesPerThread[threadIndex].clear();
    m_visiblePatchesPerThread[threadIndex].clear();
    int cameraCluster = m_renderLeaves[cameraLeaf].visCluster;

    //loop through the leaves
    for (const auto &rl : m_renderLeaves)
    {
        //if the leaf is not in the PVS - skip it
        if (!HasRenderFlag(Q3RenderSkipPVS) && !ClusterVisible(cameraCluster, rl.visCluster))
            continue;

        //if this leaf does not lie in the frustum - skip it
        if (!HasRenderFlag(Q3RenderSkipFC) && !m_frustum.BoxInFrustum(rl.boundingBoxVertices))
            continue;

        //loop through faces in this leaf and them to visibility set
        for (int j = 0; j < rl.numFaces; ++j)
        {
            int idx = leafFaces[rl.firstFace + j].face;
            // determine if this face should be rendered by current thread - we do this to avoid "blinking" if same face ends up in different threads each frame
            // this is also faster than forcing the threads to wait for each other with mutexes and keeping global visibility lists!
            bool idxInRange = (idx >= threadIndex * m_facesPerThread) && (idx < (threadIndex + 1) * m_facesPerThread);
            Q3FaceRenderable *face = &m_renderFaces[idx];

            if ((HasRenderFlag(Q3RenderSkipMissingTex) && !m_textures[faces[idx].texture]) || !idxInRange)
                continue;

            if (face->type == FaceTypePolygon || face->type == FaceTypeMesh)
            {
                m_visibleFacesPerThread[threadIndex].insert(face);
            }

            if (face->type == FaceTypePatch)
            {
                m_visiblePatchesPerThread[threadIndex].insert(idx);
            }
        }
    }
}

void Q3BspMap::ToggleRenderFlag(int flag)
{
    m_renderFlags ^= flag;
    bool set = HasRenderFlag(flag);

    switch (flag)
    {
    case Q3RenderShowWireframe:
        m_facesPipeline.mode = set ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        m_patchPipeline.mode = set ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        vkDeviceWaitIdle(g_renderContext.Device().logical);
        RebuildPipeline();
        break;
    case Q3RenderShowLightmaps:
        m_pc.renderLightmaps = set ? 1 : 0;
        break;
    case Q3RenderUseLightmaps:
        m_pc.useLightmaps = set ? 1 : 0;
        break;
    case Q3RenderAlphaTest:
        m_pc.useAlphaTest = set ? 1 : 0;
        break;
    default:
        break;
    }
}

void Q3BspMap::LoadTextures()
{
    int numTextures = header.direntries[Textures].length / sizeof(Q3BspTextureLump);

    m_textures.resize(numTextures);

    // load the textures from file (determine wheter it's a jpg or tga)
    for (const auto &f : faces)
    {
        if (m_textures[f.texture])
            continue;

        std::string nameJPG = textures[f.texture].name;

        nameJPG.append(".jpg");

        m_textures[f.texture] = TextureManager::GetInstance()->LoadTexture(nameJPG.c_str());

        if (m_textures[f.texture] == nullptr)
        {
            std::string nameTGA = textures[f.texture].name;
            nameTGA.append(".tga");

            m_textures[f.texture] = TextureManager::GetInstance()->LoadTexture(nameTGA.c_str());

            if (m_textures[f.texture] == nullptr)
            {
                std::stringstream sstream;
                sstream << "Missing texture: " << nameTGA.c_str() << "\n";
                LOG_MESSAGE(sstream.str().c_str());
            }
        }
    }
}

void Q3BspMap::LoadLightmaps()
{
    m_lightmapTextures = new vk::Texture[lightMaps.size()];

    // optional: change gamma settings of the lightmaps (make them brighter)
    SetLightmapGamma(2.5f);

    // few GPUs support true 24bit textures, so we need to convert lightmaps to 32bit for Vulkan to work
    unsigned char rgba_lmap[128 * 128 * 4];
    for (size_t i = 0; i < lightMaps.size(); ++i)
    {
        memset(rgba_lmap, 255, 128 * 128 * 4);
        for (int j = 0, k = 0; k < 128 * 128 * 3; j += 4, k += 3)
            memcpy(rgba_lmap + j, lightMaps[i].map + k, 3);

        // Create texture from bsp lightmap data (8 mip levels for 128x128 textures)
        m_lightmapTextures[i].mipLevels = 8;
        vk::createTexture(g_renderContext.Device(), &m_lightmapTextures[i], rgba_lmap, 128, 128);
    }

    // Create white texture for if no lightmap specified
    unsigned char white[] = { 255, 255, 255, 255 };

    vk::createTexture(g_renderContext.Device(), &m_whiteTex, white, 1, 1);
}

// tweak lightmap gamma settings
void Q3BspMap::SetLightmapGamma(float gamma)
{
    for (size_t i = 0; i < lightMaps.size(); ++i)
    {
        for (int j = 0; j < 128 * 128; ++j)
        {
            float r, g, b;

            r = lightMaps[i].map[j * 3 + 0];
            g = lightMaps[i].map[j * 3 + 1];
            b = lightMaps[i].map[j * 3 + 2];

            r *= gamma / 255.0f;
            g *= gamma / 255.0f;
            b *= gamma / 255.0f;

            float scale = 1.0f;
            float temp;
            if (r > 1.0f && (temp = (1.0f / r)) < scale) scale = temp;
            if (g > 1.0f && (temp = (1.0f / g)) < scale) scale = temp;
            if (b > 1.0f && (temp = (1.0f / b)) < scale) scale = temp;

            scale *= 255.0f;
            r *= scale;
            g *= scale;
            b *= scale;

            lightMaps[i].map[j * 3 + 0] = (unsigned char)r;
            lightMaps[i].map[j * 3 + 1] = (unsigned char)g;
            lightMaps[i].map[j * 3 + 2] = (unsigned char)b;
        }
    }
}

// create a Q3Bsp curved surface
void Q3BspMap::CreatePatch(const Q3BspFaceLump &f)
{
    Q3BspPatch *newPatch = new Q3BspPatch;

    newPatch->textureIdx  = f.texture;
    newPatch->lightmapIdx = f.lm_index;
    newPatch->width  = f.size.x;
    newPatch->height = f.size.y;

    int numPatchesWidth  = (newPatch->width  - 1) >> 1;
    int numPatchesHeight = (newPatch->height - 1) >> 1;

    newPatch->quadraticPatches.resize(numPatchesWidth*numPatchesHeight);

    // generate biquadratic patches (components that make the curved surface)
    for (int y = 0; y < numPatchesHeight; ++y)
    {
        for (int x = 0; x < numPatchesWidth; ++x)
        {
            for (int row = 0; row < 3; ++row)
            {
                for (int col = 0; col < 3; ++col)
                {
                    int patchIdx = y * numPatchesWidth + x;
                    int cpIdx = row * 3 + col;
                    newPatch->quadraticPatches[patchIdx].controlPoints[cpIdx] = vertices[f.vertex +
                                                                                         (y * 2 * newPatch->width + x * 2) +
                                                                                         row * newPatch->width + col];
                }
            }

            newPatch->quadraticPatches[y * numPatchesWidth + x].Tesselate(Q3BspMap::s_tesselationLevel);
        }
    }

    m_patches.push_back(newPatch);
}

void Q3BspMap::Draw(int threadIndex, VkCommandBufferInheritanceInfo inheritanceInfo)
{
    // no visible patches nor faces for this thread - bail out
    if (m_visibleFacesPerThread[threadIndex].empty() && m_visiblePatchesPerThread[threadIndex].empty())
        return;

    VkBuffer vertexBuffers[] = { m_faceVertexBuffer.buffer, m_patchVertexBuffer.buffer };
    VkDeviceSize offsets[] = { 0 };

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    beginInfo.pInheritanceInfo = &inheritanceInfo;

    int frameIdx = g_renderContext.ActiveFrame();
    VK_VERIFY(vkBeginCommandBuffer(m_commandBuffers[frameIdx][threadIndex], &beginInfo));
    vkCmdSetViewport(m_commandBuffers[frameIdx][threadIndex], 0, 1, &g_renderContext.Viewport());
    vkCmdSetScissor(m_commandBuffers[frameIdx][threadIndex], 0, 1, &g_renderContext.Scissor());

    // draw regular faces
    vkCmdPushConstants(m_commandBuffers[frameIdx][threadIndex], m_facesPipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(BspPushConstants), &m_pc);
    vkCmdBindPipeline(m_commandBuffers[frameIdx][threadIndex], VK_PIPELINE_BIND_POINT_GRAPHICS, m_facesPipeline.pipeline);
    vkCmdBindVertexBuffers(m_commandBuffers[frameIdx][threadIndex], 0, 1, vertexBuffers, offsets);
    // quake 3 bsp requires uint32 for index type - 16 is too small
    vkCmdBindIndexBuffer(m_commandBuffers[frameIdx][threadIndex], m_faceIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    for (auto &f : m_visibleFacesPerThread[threadIndex])
    {
        FaceBuffers &fb = m_renderBuffers.m_faceBuffers[f->index];
        vkCmdBindDescriptorSets(m_commandBuffers[frameIdx][threadIndex], VK_PIPELINE_BIND_POINT_GRAPHICS, m_facesPipeline.layout, 0, 1, &fb.descriptor.set, 0, nullptr);
        vkCmdDrawIndexed(m_commandBuffers[frameIdx][threadIndex], fb.indexCount, 1, fb.indexOffset, fb.vertexOffset, 0);
    }

    // draw patches
    vkCmdPushConstants(m_commandBuffers[frameIdx][threadIndex], m_patchPipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(BspPushConstants), &m_pc);
    vkCmdBindPipeline(m_commandBuffers[frameIdx][threadIndex], VK_PIPELINE_BIND_POINT_GRAPHICS, m_patchPipeline.pipeline);
    vkCmdBindVertexBuffers(m_commandBuffers[frameIdx][threadIndex], 0, 1, &vertexBuffers[1], offsets);
    // quake 3 bsp requires uint32 for index type - 16 is too small
    vkCmdBindIndexBuffer(m_commandBuffers[frameIdx][threadIndex], m_patchIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    for (auto &pi : m_visiblePatchesPerThread[threadIndex])
    {
        vkCmdBindDescriptorSets(m_commandBuffers[frameIdx][threadIndex], VK_PIPELINE_BIND_POINT_GRAPHICS, m_patchPipeline.layout, 0, 1, &m_renderBuffers.m_patchBuffers[pi][0].descriptor.set, 0, nullptr);
        for (auto &p : m_renderBuffers.m_patchBuffers[pi])
        {
            vkCmdDrawIndexed(m_commandBuffers[frameIdx][threadIndex], p.indexCount, 1, p.indexOffset, p.vertexOffset, 0);
        }
    }

    VK_VERIFY(vkEndCommandBuffer(m_commandBuffers[frameIdx][threadIndex]));
}

void Q3BspMap::CreateDescriptorsForFace(const Q3BspFaceLump &face, int idx, int vertexOffset, int indexOffset)
{
    if (face.type == FaceTypeBillboard)
        return;

    auto &faceBuffer = m_renderBuffers.m_faceBuffers[idx];
    faceBuffer.descriptor.setLayout = m_dsLayout;
    faceBuffer.descriptor.pool = m_descriptorPool;
    faceBuffer.vertexCount = face.n_vertexes;
    faceBuffer.indexCount  = face.n_meshverts;
    faceBuffer.vertexOffset = vertexOffset;
    faceBuffer.indexOffset = indexOffset;

    // check if both the texture and lightmap exist and if not - replace them with missing/white texture stubs
    const vk::Texture *colorTex = m_textures[faces[idx].texture] ? *m_textures[faces[idx].texture] : *m_missingTex;
    const vk::Texture &lmap = faces[idx].lm_index >= 0 ? m_lightmapTextures[faces[idx].lm_index] : m_whiteTex;

    const vk::Texture *textureSet[2] = { colorTex, &lmap };
    CreateDescriptor(textureSet, &faceBuffer.descriptor);
}

void Q3BspMap::CreateDescriptorsForPatch(int idx, int &vertexOffset, int &indexOffset, std::vector<Q3BspBiquadPatch*> &vertexData, std::vector<int> &indexData)
{
    auto *patch = m_patches[idx];
    int numPatches = (int)patch->quadraticPatches.size();

    // check if both the texture and lightmap exist and if not - replace them with missing/white texture stubs
    const vk::Texture *colorTex = m_textures[patch->textureIdx] ? *m_textures[patch->textureIdx] : *m_missingTex;
    const vk::Texture &lmap = patch->lightmapIdx >= 0 ? m_lightmapTextures[patch->lightmapIdx] : m_whiteTex;
    const vk::Texture *textureSet[] = { colorTex, &lmap };

    // create Vulkan descriptor
    vk::Descriptor patchDescriptor;
    patchDescriptor.setLayout = m_dsLayout;
    patchDescriptor.pool = m_descriptorPool;
    CreateDescriptor(textureSet, &patchDescriptor);

    for (int i = 0; i < numPatches; i++)
    {
        // shorthand references so the code is easier to read
        auto &biquadPatch = patch->quadraticPatches[i];
        auto &patchBuffer = m_renderBuffers.m_patchBuffers[idx];
        int numVerts  = (int)biquadPatch.m_vertices.size();
        int tessLevel = biquadPatch.m_tesselationLevel;
        vertexData.push_back(&biquadPatch);

        for (int row = 0; row < tessLevel; ++row)
        {
            FaceBuffers pb;
            pb.descriptor = patchDescriptor;
            pb.vertexCount = numVerts;
            pb.indexCount  = 2 * (tessLevel + 1);
            pb.vertexOffset = vertexOffset;
            pb.indexOffset  = indexOffset;
            indexOffset += pb.indexCount;
            indexData.push_back(row * pb.indexCount);

            patchBuffer.emplace_back(pb);
        }

        vertexOffset += numVerts;
    }
}

void Q3BspMap::CreateFaceBuffers(const std::vector<Q3BspFaceLump*> &faceData, int vertexCount, int indexCount)
{
    vk::Buffer vertexStaging, indexStaging;
    size_t vertexOffset = 0, indexOffset  = 0;

    // staging buffer for vertex data
    vk::createStagingBuffer(g_renderContext.Device(), sizeof(Q3BspVertexLump) * vertexCount, &vertexStaging);
    // staging buffer for index data
    vk::createStagingBuffer(g_renderContext.Device(), sizeof(Q3BspMeshVertLump) * indexCount, &indexStaging);

    void *dstV, *dstI;
    vmaMapMemory(g_renderContext.Device().allocator, vertexStaging.allocation, &dstV);
    vmaMapMemory(g_renderContext.Device().allocator, indexStaging.allocation, &dstI);
    for (auto &f : faceData)
    {
        memcpy((char*)dstV + vertexOffset, &(vertices[f->vertex].position), sizeof(Q3BspVertexLump) * f->n_vertexes);
        memcpy((char*)dstI + indexOffset, &meshVertices[f->meshvert], sizeof(Q3BspMeshVertLump) * f->n_meshverts);

        vertexOffset += sizeof(Q3BspVertexLump) * f->n_vertexes;
        indexOffset  += sizeof(Q3BspMeshVertLump) * f->n_meshverts;
    }
    vmaUnmapMemory(g_renderContext.Device().allocator, vertexStaging.allocation);
    vmaUnmapMemory(g_renderContext.Device().allocator, indexStaging.allocation);

    // create rendering buffers
    vk::createVertexBufferStaged(g_renderContext.Device(), sizeof(Q3BspVertexLump) * vertexCount, vertexStaging, &m_faceVertexBuffer);
     vk::createIndexBufferStaged(g_renderContext.Device(), sizeof(Q3BspMeshVertLump) * indexCount, indexStaging, &m_faceIndexBuffer);

    freeBuffer(g_renderContext.Device(), vertexStaging);
    freeBuffer(g_renderContext.Device(), indexStaging);
}

void Q3BspMap::CreatePatchBuffers(const std::vector<Q3BspBiquadPatch*> &patchData, int vertexCount, int indexCount)
{
    vk::Buffer vertexStaging, indexStaging;
    size_t vertexOffset = 0, indexOffset = 0;

    // staging buffer for vertex data
    vk::createStagingBuffer(g_renderContext.Device(), sizeof(Q3BspVertexLump) * vertexCount, &vertexStaging);
    // staging buffer for index data
    vk::createStagingBuffer(g_renderContext.Device(), sizeof(Q3BspMeshVertLump) * indexCount, &indexStaging);

    void *dstV, *dstI;
    vmaMapMemory(g_renderContext.Device().allocator, vertexStaging.allocation, &dstV);
    vmaMapMemory(g_renderContext.Device().allocator, indexStaging.allocation, &dstI);
    for (auto &p : patchData)
    {
        memcpy((char*)dstV + vertexOffset, &p->m_vertices[0].position, sizeof(Q3BspVertexLump) * p->m_vertices.size());
        vertexOffset += sizeof(Q3BspVertexLump) * p->m_vertices.size();

        int tessLevel = p->m_tesselationLevel;
        int indexCount = 2 * (tessLevel + 1);
        for (int row = 0; row < tessLevel; ++row)
        {
            memcpy((char*)dstI + indexOffset, &p->m_indices[row * indexCount], sizeof(Q3BspMeshVertLump) * indexCount);
            indexOffset += sizeof(Q3BspMeshVertLump) * indexCount;
        }
    }
    vmaUnmapMemory(g_renderContext.Device().allocator, vertexStaging.allocation);
    vmaUnmapMemory(g_renderContext.Device().allocator, indexStaging.allocation);

    // create rendering buffers
    vk::createVertexBufferStaged(g_renderContext.Device(), sizeof(Q3BspVertexLump) * vertexCount, vertexStaging, &m_patchVertexBuffer);
     vk::createIndexBufferStaged(g_renderContext.Device(), sizeof(Q3BspMeshVertLump) * indexCount, indexStaging, &m_patchIndexBuffer);

    freeBuffer(g_renderContext.Device(), vertexStaging);
    freeBuffer(g_renderContext.Device(), indexStaging);
}

void Q3BspMap::CreateDescriptorSetLayout()
{
    VkDescriptorSetLayoutBinding uboLayoutBinding = {};
    uboLayoutBinding.binding = 0;
    uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboLayoutBinding.descriptorCount = 1;
    uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    uboLayoutBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutBinding samplerLayoutBinding = {};
    samplerLayoutBinding.binding = 1;
    samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerLayoutBinding.descriptorCount = 1;
    samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    samplerLayoutBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutBinding lightmapSamplerLayoutBinding = {};
    lightmapSamplerLayoutBinding.binding = 2;
    lightmapSamplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    lightmapSamplerLayoutBinding.descriptorCount = 1;
    lightmapSamplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    lightmapSamplerLayoutBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutBinding bindings[] = { uboLayoutBinding, samplerLayoutBinding, lightmapSamplerLayoutBinding };
    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;

    VK_VERIFY(vkCreateDescriptorSetLayout(g_renderContext.Device().logical, &layoutInfo, nullptr, &m_dsLayout));
}

void Q3BspMap::CreateDescriptorPool(uint32_t numDescriptors)
{
    VkDescriptorPoolSize poolSizes[3];
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = numDescriptors;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = numDescriptors;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[2].descriptorCount = numDescriptors;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = numDescriptors;

    VK_VERIFY(vkCreateDescriptorPool(g_renderContext.Device().logical, &poolInfo, nullptr, &m_descriptorPool));
}

void Q3BspMap::CreateDescriptor(const vk::Texture **textures, vk::Descriptor *descriptor)
{
    // create descriptor set
    VK_VERIFY(vk::createDescriptorSet(g_renderContext.Device(), descriptor));
    VkDescriptorBufferInfo bufferInfo = {};
    bufferInfo.offset = 0;
    bufferInfo.buffer = m_renderBuffers.uniformBuffer.buffer;
    bufferInfo.range = sizeof(UniformBufferObject);

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = textures[0]->imageView;
    imageInfo.sampler = textures[0]->sampler;

    VkDescriptorImageInfo lightmapInfo = {};
    lightmapInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    lightmapInfo.imageView = textures[1]->imageView;
    lightmapInfo.sampler = textures[1]->sampler;

    VkWriteDescriptorSet descriptorWrites[3];
    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = descriptor->set;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pBufferInfo = &bufferInfo;
    descriptorWrites[0].pImageInfo = nullptr;
    descriptorWrites[0].pTexelBufferView = nullptr;
    descriptorWrites[0].pNext = nullptr;
    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstSet = descriptor->set;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pBufferInfo = nullptr;
    descriptorWrites[1].pImageInfo = &imageInfo;
    descriptorWrites[1].pTexelBufferView = nullptr;
    descriptorWrites[1].pNext = nullptr;
    descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[2].dstSet = descriptor->set;
    descriptorWrites[2].dstBinding = 2;
    descriptorWrites[2].dstArrayElement = 0;
    descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[2].descriptorCount = 1;
    descriptorWrites[2].pBufferInfo = nullptr;
    descriptorWrites[2].pImageInfo = &lightmapInfo;
    descriptorWrites[2].pTexelBufferView = nullptr;
    descriptorWrites[2].pNext = nullptr;

    vkUpdateDescriptorSets(g_renderContext.Device().logical, 3, descriptorWrites, 0, nullptr);
}
