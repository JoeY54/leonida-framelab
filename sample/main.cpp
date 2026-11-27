/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define VMA_IMPLEMENTATION

#include <glm/glm.hpp>
#include <vulkan/vk_enum_string_helper.h>

#include "nvvkhl/appbase_vk.hpp"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "nvh/primitives.hpp"
#include "nvvk/context_vk.hpp"
#include "nvvk/descriptorsets_vk.hpp"
#include "nvvk/dynamicrendering_vk.hpp"
#include "nvvk/images_vk.hpp"
#include "nvvk/pipeline_vk.hpp"
#include "nvvk/memallocator_vma_vk.hpp"
#include "nvvkhl/gbuffer.hpp"

#include "streamline_wrapper.hpp"

#include "shaders/common.h"
#include "_autogen/post.vert.h"
#include "_autogen/post.frag.h"
#include "_autogen/scene.vert.h"
#include "_autogen/scene.frag.h"

#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
#include "public_vulkan_fg.hpp"
#else
#include "direct_ngx_fg.hpp"
#endif
#include <filesystem>
#include <chrono>
#include <atomic>
static bool g_directFG = true, g_verifyDirectFG = false;
static unsigned g_frameLimit=0;
static int g_missingDepthFrame=-1;
static bool g_validation=false;
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
enum class SdkHandoff { none, sameFamily, crossFamily };
static SdkHandoff g_sdkHandoff=SdkHandoff::none;
static bool g_cancelFirstFgResize=false;
static int g_cancelEvaluateFrame=-1;
static bool g_cancelledEvaluate=false;
#endif
static bool g_initialVsync=true;
static std::atomic<unsigned> g_validationErrors{0},g_validationWarnings{0};
static VKAPI_ATTR VkBool32 VKAPI_CALL sampleValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void*) {
  if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)++g_validationErrors;else if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)++g_validationWarnings;
  printf("SAMPLE_VALIDATION severity=%u %s\n",unsigned(severity),data&&data->pMessage?data->pMessage:"(no message)");return VK_FALSE;
}
static bool g_verifyFG = false;
static bool g_srOnly = false;
static bool useManagedSwapchain() { return g_directFG || g_srOnly; }
static int SAMPLE_WIDTH  = 1920;
static int SAMPLE_HEIGHT = 1080;
static std::filesystem::path g_ngxRuntimeDir, g_directLogDir, g_streamlineLogDir;

static std::filesystem::path pathFromEnvironment(const wchar_t* name)
{
  const auto size=GetEnvironmentVariableW(name,nullptr,0);
  if(!size) return {};
  std::wstring value(size,L'\0');
  const auto length=GetEnvironmentVariableW(name,value.data(),size);
  if(!length || length>=size) throw std::runtime_error("Could not read runtime path from the environment");
  value.resize(length);
  return std::filesystem::absolute(std::filesystem::path(value));
}

static void configureRuntime()
{
  auto logRoot=pathFromEnvironment(L"FRAMELAB_LOG_DIR");
  if(logRoot.empty()) logRoot=std::filesystem::current_path() / L"framelab-logs";
  g_streamlineLogDir=logRoot / L"streamline";
  std::filesystem::create_directories(g_streamlineLogDir);
  if(!g_directFG) return;

  g_directLogDir=logRoot / L"direct";
  std::filesystem::create_directories(g_directLogDir);
  g_ngxRuntimeDir=pathFromEnvironment(L"FRAMELAB_NGX_RUNTIME_DIR");
  if(g_ngxRuntimeDir.empty()) {
    std::wstring executable(32768,L'\0');
    const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    if(!length || length>=executable.size()) throw std::runtime_error("Could not locate the sample executable");
    executable.resize(length);
    g_ngxRuntimeDir=std::filesystem::path(executable).parent_path();
  }
  if(!std::filesystem::is_directory(g_ngxRuntimeDir))
    throw std::runtime_error("FRAMELAB_NGX_RUNTIME_DIR must name an existing directory");

  const auto proxyPath=pathFromEnvironment(L"FRAMELAB_SM86_PROXY");
  if(!proxyPath.empty()) {
    const HMODULE proxy=LoadLibraryW(proxyPath.c_str());
    std::printf("SAMPLE_SM86_PROXY loaded=%d error=%lu\n",proxy!=nullptr,proxy?0:GetLastError());
    if(!proxy) throw std::runtime_error("Could not load the configured FRAMELAB_SM86_PROXY library");
  }
}

// Streamline features to enable
static const sl::Feature SL_FEATURES[] = {
#ifdef SAMPLE_HAS_PCL
    sl::kFeaturePCL,  // Markers moved from Reflex to PCL in modern Streamline.
#endif
    sl::kFeatureReflex,  // Reflex is required for DLSS Frame Generation
    sl::kFeatureDLSS,    // DLSS Super Resolution
    sl::kFeatureDLSS_G,  // DLSS Frame Generation
};

static const VkFormat SAMPLE_DEPTH_FORMAT =
    VK_FORMAT_D24_UNORM_S8_UINT;  // Streamline only supports VK_FORMAT_D24_UNORM_S8_UINT and VK_FORMAT_D32_SFLOAT currently
static const VkFormat SAMPLE_COLOR_FORMATS[] = {
    VK_FORMAT_R16G16B16A16_SFLOAT,  // Color
    VK_FORMAT_R16G16_SFLOAT  // Motion vectors (must be in format VK_FORMAT_R16G16_SFLOAT or VK_FORMAT_R32G32_SFLOAT)
};

#define USE_D3D_CLIP_SPACE 0

class StreamlineSample : public nvvkhl::AppBaseVk
{
public:
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
  PublicVulkanFg directFG;
  VkCommandBuffer sdkConsumerCommands{};
#else
  DirectNgxFG directFG;
#endif
  unsigned directRealPresents=0,directGeneratedPresents=0;
  bool directReady=false;
  VkSurfaceKHR directSurface{};
  std::vector<bool> directSwapSeen;
  std::vector<VkSemaphore> srPresentReady;

  void releaseSrPresentSemaphores()
  {
    for(auto semaphore : srPresentReady) vkDestroySemaphore(m_device,semaphore,nullptr);
    srPresentReady.clear();
  }
  bool directRequestedVsync=true;
  bool directSwapchainDirty=false;
  VkPresentModeKHR directPresentMode=VK_PRESENT_MODE_FIFO_KHR;
  const char* directPresentModeName() const
  {
    switch(directPresentMode){
      case VK_PRESENT_MODE_IMMEDIATE_KHR:return "Immediate";
      case VK_PRESENT_MODE_MAILBOX_KHR:return "Mailbox";
      default:return "FIFO";
    }
  }
  void requestVsync(bool enabled)
  {
    directRequestedVsync=enabled;
    directSwapchainDirty=enabled!=m_vsync;
  }
  unsigned srSuccesses = 0;
  unsigned srErrors = 0;
  unsigned fgPresented = 0;
  unsigned fgErrors = 0;
  StreamlineSample()           = default;
  ~StreamlineSample() override = default;

  // The legacy base transitions every swapchain image before acquisition and gives
  // ImGui a depth format even though its overlay has no depth attachment. Keep the
  // direct backend self-contained and initialize acquired images lazily instead.
  void createSwapchain(const VkSurfaceKHR& surface,uint32_t width,uint32_t height,VkFormat color=VK_FORMAT_B8G8R8A8_UNORM,VkFormat depth=VK_FORMAT_UNDEFINED,bool vsync=false) override
  {
    if(!useManagedSwapchain()){AppBaseVk::createSwapchain(surface,width,height,color,depth,vsync);return;}
    directSurface=surface;m_size={width,height};m_colorFormat=color;m_depthFormat=VK_FORMAT_UNDEFINED;m_vsync=vsync;
    m_swapChain.init(m_device,m_physicalDevice,m_queue,m_graphicsQueueIndex,surface,color);
    directPresentMode=VK_PRESENT_MODE_FIFO_KHR;
    if(!vsync){
      uint32_t count=0;
      NVVK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice,surface,&count,nullptr));
      std::vector<VkPresentModeKHR> modes(count);
      NVVK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice,surface,&count,modes.data()));
      const auto supports=[&](VkPresentModeKHR mode){return std::find(modes.begin(),modes.end(),mode)!=modes.end();};
      if(supports(VK_PRESENT_MODE_IMMEDIATE_KHR))directPresentMode=VK_PRESENT_MODE_IMMEDIATE_KHR;
      else if(supports(VK_PRESENT_MODE_MAILBOX_KHR))directPresentMode=VK_PRESENT_MODE_MAILBOX_KHR;
    }
    m_swapChain.setPreferredVsyncOffMode(directPresentMode);
    m_size=m_swapChain.update(width,height,vsync);m_colorFormat=m_swapChain.getFormat();
    m_waitFences.resize(m_swapChain.getImageCount());
    for(auto& fence:m_waitFences){VkFenceCreateInfo ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};ci.flags=VK_FENCE_CREATE_SIGNALED_BIT;NVVK_CHECK(vkCreateFence(m_device,&ci,nullptr,&fence));}
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=m_cmdPool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=m_swapChain.getImageCount();
    m_commandBuffers.resize(ai.commandBufferCount);NVVK_CHECK(vkAllocateCommandBuffers(m_device,&ai,m_commandBuffers.data()));
    directSwapSeen.assign(ai.commandBufferCount,false);CameraManip.setWindowSize(m_size.width,m_size.height);
    if(g_srOnly){
      // An image's acquire wait also orders the previous Present wait on that image.
      srPresentReady.resize(ai.commandBufferCount);
      const VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      for(auto& semaphore : srPresentReady)
        NVVK_CHECK(vkCreateSemaphore(m_device,&ci,nullptr,&semaphore));
    }
    directRequestedVsync=vsync;directSwapchainDirty=false;
    printf("DIRECT_VSYNC requested=%s present_mode=%s extent=%ux%u\n",vsync?"on":"off",directPresentModeName(),m_size.width,m_size.height);
  }
  void createDepthBuffer() override {if(!useManagedSwapchain())AppBaseVk::createDepthBuffer();}
  void recreateDirectSwapchain(int width,int height,bool vsync)
  {
    if(width<=0||height<=0)return;
    NVVK_CHECK(vkDeviceWaitIdle(m_device));
    const VkExtent2D previousSize=m_size;
    releaseSrPresentSemaphores();
    for(auto fence:m_waitFences)vkDestroyFence(m_device,fence,nullptr);
    vkFreeCommandBuffers(m_device,m_cmdPool,uint32_t(m_commandBuffers.size()),m_commandBuffers.data());
    m_waitFences.clear();m_commandBuffers.clear();m_swapChain.deinit();
    createSwapchain(directSurface,uint32_t(width),uint32_t(height),VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_UNDEFINED,vsync);
    // Changing presentation mode alone does not require new NGX resources.
    if(m_size.width!=previousSize.width||m_size.height!=previousSize.height)
      onResize(m_size.width,m_size.height);
    else
      directFG.reset=true;
  }
  void onFramebufferSize(int width,int height) override
  {
    if(!useManagedSwapchain()){AppBaseVk::onFramebufferSize(width,height);return;}
    recreateDirectSwapchain(width,height,directSwapchainDirty?directRequestedVsync:m_vsync);
  }
  void onKeyboard(int key,int scancode,int action,int mods) override
  {
    // GLFW key codes keep the shortcut independent of the active text layout.
    if(g_directFG&&key==GLFW_KEY_V){
      if(action==GLFW_PRESS&&!ImGui::GetIO().WantTextInput)
        requestVsync(!(directSwapchainDirty?directRequestedVsync:m_vsync));
      return;
    }
    AppBaseVk::onKeyboard(key,scancode,action,mods);
  }
  void onKeyboardChar(unsigned char key) override
  {
    if(g_directFG&&(key=='v'||key=='V'))return; // Avoid toggling twice.
    AppBaseVk::onKeyboardChar(key);
  }

  void create(const nvvkhl::AppBaseVkCreateInfo& info, sl::Result dlssSupported, sl::Result dlssgSupported, bool verifySr = false)
  {
    AppBaseVk::create(info);

    m_dlssSupported  = dlssSupported;
    m_dlssgSupported = dlssgSupported;

    // This sample only operates with a single viewport, so create a handle for viewport index zero
    m_viewportHandle = sl::ViewportHandle(0);

    // Limit to 180 FPS by default
    m_reflexOptions.frameLimitUs = 5555;

    // This sample does simulation and rendering in a single thread, so can't use markers to optimize
    m_reflexOptions.useMarkersToOptimize = false;

    if(verifySr && !g_directFG && dlssSupported == sl::Result::eOk)
      m_dlssOptions.mode = sl::DLSSMode::eMaxPerformance;

    if(g_verifyFG && dlssgSupported == sl::Result::eOk) {
      m_dlssgOptions.mode = sl::DLSSGMode::eOn;
      m_dlssgOptions.numFramesToGenerate = 1;
      m_reflexOptions.mode = sl::ReflexMode::eLowLatency;
    }
    // Set Streamline default options
    slDLSSSetOptions(m_viewportHandle, m_dlssOptions);
    if(!useManagedSwapchain()) slDLSSGSetOptions(m_viewportHandle, m_dlssgOptions);
    slReflexSetOptions(m_reflexOptions);

    if(g_directFG){m_reflexOptions.mode=sl::ReflexMode::eLowLatency;slReflexSetOptions(m_reflexOptions);directFG.initialize(info.instance,info.physicalDevice,info.device,m_queue,m_graphicsQueueIndex,g_ngxRuntimeDir,g_directLogDir);}
    m_dset  = std::make_unique<nvvk::DescriptorSetContainer>(info.device);
    m_alloc = std::make_shared<nvvk::ResourceAllocatorVma>(info.instance, info.device, info.physicalDevice);

    const VkSamplerCreateInfo samplerCreateInfo = nvvk::makeSamplerCreateInfo();
    m_defaultSampler                            = m_alloc->acquireSampler(samplerCreateInfo);

    createScene();
    createPipelines();
    createImages(getSize());
  }

  void destroy() override
  {
    vkDeviceWaitIdle(m_device);

    if(g_directFG)directFG.shutdown();
    releaseSrPresentSemaphores();
    destroyImages();
    destroyPipelines();
    destroyScene();

    m_alloc->releaseSampler(m_defaultSampler);
    m_defaultSampler = VK_NULL_HANDLE;

    m_dset.reset();
    m_alloc.reset();

    AppBaseVk::destroy();
  }

  void onResize(int width, int height) override
  {
    createImages({static_cast<uint32_t>(width), static_cast<uint32_t>(height)});
  }

  void prepareFrame() override
  {
    // FL-17 run 4, diagnostic only (never a fix): complete all of frame N's GPU work on every
    // queue before frame N+1 is recorded. Enabled only with FRAMELAB_DIAG_CROSSFRAME_WAIT=1.
    static const bool diagCrossFrameWait=[]{const char* v=std::getenv("FRAMELAB_DIAG_CROSSFRAME_WAIT");return v&&v[0]=='1';}();
    if(diagCrossFrameWait){
      NVVK_CHECK(vkDeviceWaitIdle(m_device));
      static bool announced=false;
      if(!announced){announced=true;std::printf("FL17_DIAG_CROSSFRAME_WAIT=1\n");}
    }
    if(useManagedSwapchain()){
      // Apply UI/key changes between frames, before acquiring a swapchain image.
      if(directSwapchainDirty)recreateDirectSwapchain(int(m_size.width),int(m_size.height),directRequestedVsync);
      NVVK_CHECK(vkQueueWaitIdle(m_queue)); // Protect temporal history and descriptors.
    }
    AppBaseVk::prepareFrame();

    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    drawGui();
    ImGui::Render();

    m_animationTimePrev = m_animationTime;
    m_animationTime += g_verifyDirectFG ? 1.0f/30.0f : ImGui::GetIO().DeltaTime;
  }

  void renderFrame(VkCommandBuffer& cmd, sl::FrameToken* frame)
  {
    const VkExtent2D renderSize = m_gBuffers->getSize();
    const VkExtent2D outputSize = getSize();

    const float aspectRatio  = m_gBuffers->getAspectRatio();
    const float scalingRatio = static_cast<float>(outputSize.width) / static_cast<float>(renderSize.width);

    // Get view information from camera
    const glm::mat4 viewMatrix = CameraManip.getMatrix();
    const glm::vec2 clipPlanes = CameraManip.getClipPlanes();
    glm::mat4 projMatrix =
        glm::perspectiveRH_ZO(glm::radians(CameraManip.getFov()), aspectRatio, clipPlanes.x, clipPlanes.y);
#if !USE_D3D_CLIP_SPACE
    // Flip the Y axis to convert from D3D to Vulkan:
    projMatrix[1][1] *= -1;
#endif

    glm::vec3 geye, gcenter, gup;
    CameraManip.getLookat(geye, gcenter, gup);
    glm::vec3 eye     = geye;
    glm::vec3 center  = gcenter;
    glm::vec3 up      = gup;
    glm::vec3 right   = glm::normalize(glm::cross(center - eye, up));
    glm::vec3 forward = glm::normalize(center - eye);

    // Calculate pixel jitter offset using Halton sequence
    glm::vec2 jitterOffset = {};

    if(m_dlssOptions.mode != sl::DLSSMode::eOff)
    {
      const auto halton = [](uint32_t index, uint32_t base) -> float {
        float f = 1.0f;
        float r = 0.0f;

        while(index > 0)
        {
          f *= static_cast<float>(base);
          r += static_cast<float>(index % base) / f;
          index /= base;
        }

        return r;
      };

      const uint32_t jitterPhases       = static_cast<uint32_t>(8 * scalingRatio * scalingRatio + 0.5f);
      const uint32_t jitterCurrentIndex = (*frame % jitterPhases) + 1;
      jitterOffset.x                    = halton(jitterCurrentIndex, 2) - 0.5f;
      jitterOffset.y                    = halton(jitterCurrentIndex, 3) - 0.5f;
    }

    sl::Constants directConstants;
    // Update Streamline constants
    {
      const auto matrixToSL = [](const glm::mat4& m) -> sl::float4x4 {
        // Streamline expects row-major matrices
        sl::float4x4 res;
        res.row[0].x = m[0][0];
        res.row[0].y = m[1][0];
        res.row[0].z = m[2][0];
        res.row[0].w = m[3][0];
        res.row[1].x = m[0][1];
        res.row[1].y = m[1][1];
        res.row[1].z = m[2][1];
        res.row[1].w = m[3][1];
        res.row[2].x = m[0][2];
        res.row[2].y = m[1][2];
        res.row[2].z = m[2][2];
        res.row[2].w = m[3][2];
        res.row[3].x = m[0][3];
        res.row[3].y = m[1][3];
        res.row[3].z = m[2][3];
        res.row[3].w = m[3][3];
        return res;
      };

      sl::Constants constants;
      constants.jitterOffset = sl::float2(jitterOffset.x, jitterOffset.y);
      constants.mvecScale = sl::float2(1.0f, 1.0f);  // getMotion in scene.frag calculates normalized motion vectors already, so do not need to scale

      glm::mat4 projMatrixD3D     = projMatrix;
      glm::mat4 projMatrixPrevD3D = m_projMatrixPrev;
#if USE_D3D_CLIP_SPACE
      // getMotion in scene.frag does not flip Y coordinate for D3D clip space, so do it via scaling
      constants.mvecScale.y *= -1.0f;
#else
      // Streamline expects clip space as defined in Direct3D (X left->right, Y bottom->top, Z front->back)
      // But clip space in Vulkan is upside down (X left->right, Y top->bottom, Z front-back), so need to adjust projection matrices accordingly
      projMatrixD3D[1][1] *= -1.0f;
      projMatrixPrevD3D[1][1] *= -1.0f;
#endif
      const glm::mat4 reprojectionMatrix =
          projMatrixPrevD3D * (m_viewMatrixPrev * glm::inverse(viewMatrix)) * glm::inverse(projMatrixD3D);

      constants.cameraViewToClip = matrixToSL(projMatrix);
      constants.clipToCameraView = matrixToSL(glm::inverse(projMatrix));
      constants.clipToLensClip   = matrixToSL(glm::mat4(1));
      constants.clipToPrevClip   = matrixToSL(reprojectionMatrix);
      constants.prevClipToClip   = matrixToSL(glm::inverse(reprojectionMatrix));

      constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
      constants.cameraPos           = sl::float3(eye.x, eye.y, eye.z);
      constants.cameraUp            = sl::float3(up.x, up.y, up.z);
      constants.cameraRight         = sl::float3(right.x, right.y, right.z);
      constants.cameraFwd           = sl::float3(forward.x, forward.y, forward.z);

      constants.cameraNear        = clipPlanes.x;
      constants.cameraFar         = clipPlanes.y;
      constants.cameraFOV         = glm::radians(CameraManip.getFov());
      constants.cameraAspectRatio = aspectRatio;

      constants.depthInverted         = (projMatrix[2][2] == 0.0f) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
      constants.cameraMotionIncluded  = sl::Boolean::eTrue;
      constants.motionVectors3D       = sl::Boolean::eFalse;
      constants.reset                 = sl::Boolean::eFalse;
      constants.motionVectorsJittered = sl::Boolean::eFalse;

      directConstants=constants;
      if(SL_FAILED(res, slSetConstants(constants, *frame, m_viewportHandle)))
      {
        LOGE("Streamline: Failed to set constants (%d)\n", res);
        return;
      }
    }

    // Update frame info uniform buffer
    {
      FrameInfo frameInfo;
      frameInfo.viewProj     = projMatrix * viewMatrix;
      frameInfo.viewProjPrev = m_projMatrixPrev * m_viewMatrixPrev;
      frameInfo.jitterOffset = jitterOffset * 2.0f / glm::vec2(renderSize.width, renderSize.height);
      frameInfo.camPos       = eye;
      vkCmdUpdateBuffer(cmd, m_frameInfo.buffer, 0, sizeof(frameInfo), &frameInfo);

      VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      barrier.srcAccessMask   = VK_ACCESS_MEMORY_WRITE_BIT;
      barrier.dstAccessMask   = VK_ACCESS_UNIFORM_READ_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    // Scene rendering
    {
      const auto prepareAttachment=[&](VkImage image,VkImageLayout layout,VkImageAspectFlags aspects){
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
        if(g_directFG&&directFG.willSplit()) {
          // Discard the previous contents, while still ordering the layout write after
          // prior producer stores and consumer reads on the reused allocations.
          VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
          barrier.srcAccessMask=barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
          barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
          barrier.newLayout=layout;
          barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
          barrier.image=image;
          barrier.subresourceRange={aspects,0,1,0,1};
          vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               0,0,nullptr,0,nullptr,1,&barrier);
          return;
        }
#endif
        nvvk::cmdBarrierImageLayout(cmd,image,VK_IMAGE_LAYOUT_UNDEFINED,layout,aspects);
      };
      std::vector<VkImageView> colorImageViews;
      for(uint32_t i = 0; i < std::size(SAMPLE_COLOR_FORMATS); i++)
      {
        colorImageViews.push_back(m_gBuffers->getColorImageView(i));
        prepareAttachment(m_gBuffers->getColorImage(i),VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_ASPECT_COLOR_BIT);
      }
      prepareAttachment(m_gBuffers->getDepthImage(),VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT);

      nvvk::createRenderingInfo renderingInfo({{0, 0}, renderSize}, colorImageViews, m_gBuffers->getDepthImageView(),
                                              VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_CLEAR);
      renderingInfo.colorAttachments[0].clearValue = {1.0f, 0.0f, 1.0f, 1.0f};  // Color
      renderingInfo.colorAttachments[1].clearValue = {0.0f, 0.0f, 0.0f, 0.0f};  // Motion vectors
      renderingInfo.pStencilAttachment             = nullptr;
      vkCmdBeginRendering(cmd, &renderingInfo);

#if USE_D3D_CLIP_SPACE
      // Flip viewport to simulate D3D clip space (this is supported since the VK_KHR_maintenance1 extension)
      const VkViewport viewport = {0.0f,
                                   static_cast<float>(renderSize.height),
                                   static_cast<float>(renderSize.width),
                                   -static_cast<float>(renderSize.height),
                                   0.0f,
                                   1.0f};
#else
      const VkViewport viewport = {
          0.0f, 0.0f, static_cast<float>(renderSize.width), static_cast<float>(renderSize.height), 0.0f, 1.0f};
#endif
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      const VkRect2D scissor = {{0, 0}, {renderSize.width, renderSize.height}};
      vkCmdSetScissor(cmd, 0, 1, &scissor);

      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_dset->getPipeLayout(), 0, 1,
                              m_dset->getSets(), 0, nullptr);

      for(const Node& node : m_sceneNodes)
      {
        NodeInfo nodeInfo;
        nodeInfo.model = nodeInfo.modelPrev = node.localMatrix();
        if(node.motion)
        {
          nodeInfo.model *= glm::translate(glm::mat4(1.f), {-sinf(fmodf(m_animationTime, glm::two_pi<float>())), 0.0f, 0.0f});
          nodeInfo.modelPrev *=
              glm::translate(glm::mat4(1.f), {-sinf(fmodf(m_animationTimePrev, glm::two_pi<float>())), 0.0f, 0.0f});
        }
        nodeInfo.color = glm::vec3(sin(glm::vec3(1.33333f, 2.33333f, 3.33333f) * static_cast<float>(node.material)) * 0.5f + 0.5f);
        vkCmdPushConstants(cmd, m_dset->getPipeLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(nodeInfo), &nodeInfo);

        const VkDeviceSize offsets = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_sceneMeshes[node.mesh].verticesBuffer.buffer, &offsets);
        vkCmdBindIndexBuffer(cmd, m_sceneMeshes[node.mesh].trianglesBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

        vkCmdDrawIndexed(cmd, static_cast<uint32_t>(m_sceneMeshes[node.mesh].triangles.size() * 3), 1, 0, 0, 0);
      }

      vkCmdEndRendering(cmd);
    }

    // Tag current state of resources needed for DLSS Super Resolution
    {
      sl::Resource colorInputResource(sl::ResourceType::eTex2d, m_gBuffers->getColorImage(0), nullptr,
                                      m_gBuffers->getColorImageView(0), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      colorInputResource.width        = renderSize.width;
      colorInputResource.height       = renderSize.height;
      colorInputResource.nativeFormat = m_gBuffers->getColorFormat(0);
      colorInputResource.mipLevels    = 1;
      colorInputResource.arrayLayers  = 1;
      colorInputResource.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                                 | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // See nvvkhl::GBuffer::create

      sl::Resource colorOutputResource(sl::ResourceType::eTex2d, m_outputImage.image, nullptr, m_outputImageView,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      colorOutputResource.width        = outputSize.width;
      colorOutputResource.height       = outputSize.height;
      colorOutputResource.nativeFormat = SAMPLE_COLOR_FORMATS[0];
      colorOutputResource.mipLevels    = 1;
      colorOutputResource.arrayLayers  = 1;
      colorOutputResource.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;  // See createImages below

      sl::Resource depthResource(sl::ResourceType::eTex2d, m_gBuffers->getDepthImage(), nullptr,
                                 m_gBuffers->getDepthImageView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
      depthResource.width        = renderSize.width;
      depthResource.height       = renderSize.height;
      depthResource.nativeFormat = m_gBuffers->getDepthFormat();
      depthResource.mipLevels    = 1;
      depthResource.arrayLayers  = 1;
      depthResource.usage        = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // See nvvkhl::GBuffer::create

      sl::Resource motionVectorsResource(sl::ResourceType::eTex2d, m_gBuffers->getColorImage(1), nullptr,
                                         m_gBuffers->getColorImageView(1), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      motionVectorsResource.width        = renderSize.width;
      motionVectorsResource.height       = renderSize.height;
      motionVectorsResource.nativeFormat = m_gBuffers->getColorFormat(1);
      motionVectorsResource.mipLevels    = 1;
      motionVectorsResource.arrayLayers  = 1;
      motionVectorsResource.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // See nvvkhl::GBuffer::create

      const sl::ResourceTag tags[] = {
          sl::ResourceTag(&colorInputResource, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent),
          sl::ResourceTag(&colorOutputResource, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent),
          sl::ResourceTag(&depthResource, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
          sl::ResourceTag(&motionVectorsResource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
      };

      slSetTag(m_viewportHandle, tags, static_cast<uint32_t>(std::size(tags)), cmd);
    }

    if(m_dlssOptions.mode != sl::DLSSMode::eOff)
    {
      nvvk::cmdBarrierImageLayout(cmd, m_outputImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

      const sl::BaseStructure* inputs[] = {&m_viewportHandle};

      if(SL_FAILED(res, slEvaluateFeature(sl::kFeatureDLSS, *frame, inputs, static_cast<uint32_t>(std::size(inputs)), cmd)))
      {
        ++srErrors;
        LOGE("Streamline: Failed to evaluate DLSS Super Resolution (%d)\n", res);
      }
      else
      {
        ++srSuccesses;
      }
    }
    else
    {
      nvvk::cmdBarrierImageLayout(cmd, m_outputImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

      // When DLSS Super Resolution is off, 'm_outputImage' already points to 'm_gBuffers->getColorImage()', so no additional processing needed here
    }

    directReady=false;
    if(g_directFG){
      auto inputs=frameInputs(cmd,directConstants);
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
      if(directFG.willSplit()) {
        directFG.splitProducer(cmd,inputs);
        sdkConsumerCommands=createTempCmdBuffer();
        cmd=sdkConsumerCommands;
      }
#endif
      if(g_missingDepthFrame>=0&&inputs.frame.frameIndex==std::uint64_t(g_missingDepthFrame))inputs.frame.depth={};
      directReady=directFG.evaluate(cmd,inputs);
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
      if(g_cancelEvaluateFrame>=0&&inputs.frame.frameIndex==uint64_t(g_cancelEvaluateFrame)) {
        // The producer was submitted on the other queue. Keep its semaphore wait,
        // but discard the unsubmitted consumer recording before cancelling at the SDK.
        if(!sdkConsumerCommands)throw std::runtime_error("cancellation needs a separate consumer recording");
        vkFreeCommandBuffers(m_device,m_cmdPool,1,&sdkConsumerCommands);
        if(!directFG.cancelUnsubmitted(true))
          throw std::runtime_error("native Evaluate cancellation did not exercise a recording");
        sdkConsumerCommands=createTempCmdBuffer();
        cmd=sdkConsumerCommands;
        directReady=false;
        g_cancelledEvaluate=true;
        std::printf("PUBLIC_VULKAN_FG native_evaluate_cancelled=1 source_frame=%llu\n",
                    static_cast<unsigned long long>(inputs.frame.frameIndex));
      }
#endif
    }
    renderOutput(cmd,directReady);

    // Tag current state of additional resources needed for DLSS Frame Generation
    {
      sl::Resource colorResource(sl::ResourceType::eTex2d, m_outputImage.image, nullptr, m_outputImageView,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      colorResource.width        = outputSize.width;
      colorResource.height       = outputSize.height;
      colorResource.nativeFormat = SAMPLE_COLOR_FORMATS[0];
      colorResource.mipLevels    = 1;
      colorResource.arrayLayers  = 1;
      colorResource.usage        = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;  // See createImages below

      const sl::ResourceTag tags[] = {
          sl::ResourceTag(&colorResource, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent),
      };

      slSetTag(m_viewportHandle, tags, static_cast<uint32_t>(std::size(tags)), cmd);
    }

    // Update previous matrices for next frame
    m_projMatrixPrev = projMatrix;
    m_viewMatrixPrev = viewMatrix;
  }

  frame_inputs::VulkanFrameInputs frameInputs(VkCommandBuffer cmd,const sl::Constants& constants)
  {
    namespace fi=frame_inputs;
    fi::VulkanFrameInputs inputs;auto& f=inputs.frame;
    f.stream=f.device=1;f.generation=m_inputGeneration;f.frameIndex=m_sourceFrameIndex++;
    const auto render=m_gBuffers->getSize(),output=getSize();
    f.renderExtent={render.width,render.height};f.outputExtent={output.width,output.height};
    f.deltaSeconds=g_verifyDirectFG?1./30.:double(ImGui::GetIO().DeltaTime);
    f.resetHistory=(directFG.reset||f.deltaSeconds>.25)?fi::Reset::discontinuity:fi::Reset::none;
    const auto image=[&](fi::Id id,fi::Format format,fi::State state,fi::Extent size){
      return fi::Image{id,f.frameIndex,1,format,size,state,fi::Provenance::synthetic_renderer,m_frameOwner};
    };
    f.color=image(1,fi::Format::rgba16_float,fi::State::shader_read,f.outputExtent);
    f.depth=image(2,fi::Format::d24s8,fi::State::depth_attachment,f.renderExtent);
    f.motionVectors=image(3,fi::Format::rg16_float,fi::State::color_attachment,f.renderExtent);
    // The current direct backend supports native size with SR off. Never lease an
    // unrelated GBuffer as the owner of a separately allocated SR output image.
    if(m_dlssOptions.mode!=sl::DLSSMode::eOff)f.color.allocationLease.reset();
    auto& c=f.camera;
    const auto matrix=[](const sl::float4x4& from){fi::Matrix result;memcpy(result.data(),&from,64);return result;};
    c.viewToClip=matrix(constants.cameraViewToClip);c.clipToView=matrix(constants.clipToCameraView);
    c.clipToLens=matrix(constants.clipToLensClip);c.clipToPrevious=matrix(constants.clipToPrevClip);c.previousToClip=matrix(constants.prevClipToClip);
    c.position={constants.cameraPos.x,constants.cameraPos.y,constants.cameraPos.z};
    c.up={constants.cameraUp.x,constants.cameraUp.y,constants.cameraUp.z};c.right={constants.cameraRight.x,constants.cameraRight.y,constants.cameraRight.z};c.forward={constants.cameraFwd.x,constants.cameraFwd.y,constants.cameraFwd.z};
    c.jitterPixels={constants.jitterOffset.x,constants.jitterOffset.y};c.motionVectorScale={constants.mvecScale.x,constants.mvecScale.y};
    c.nearPlane=constants.cameraNear;c.farPlane=constants.cameraFar;c.fovRadians=constants.cameraFOV;c.aspect=constants.cameraAspectRatio;
    // Preserve the baseline sample's convention pair explicitly, rather than silently
    // relabeling its Vulkan projection and D3D-oriented reprojection as identical.
    c.projectionSpace=fi::ClipSpace::y_down_zero_to_one;c.reprojectionSpace=fi::ClipSpace::y_up_zero_to_one;
    c.motion=fi::MotionConvention::previous_minus_current_uv;c.verified=true;
    c.depthInverted=constants.depthInverted==sl::Boolean::eTrue;c.cameraMotionIncluded=constants.cameraMotionIncluded==sl::Boolean::eTrue;
    f.sync.completion=fi::Completion::same_command_stream;f.sync.queue=1;f.sync.commandStream=f.frameIndex+1;
    inputs.deviceId=inputs.queueId=1;inputs.commandStreamId=f.sync.commandStream;
    inputs.device=m_device;inputs.queue=m_queue;inputs.queueFamily=m_graphicsQueueIndex;inputs.producerCommands=cmd;
    inputs.color={1,m_outputImage.image,m_outputImageView,SAMPLE_COLOR_FORMATS[0],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_ASPECT_COLOR_BIT,m_graphicsQueueIndex,f.outputExtent};
    inputs.depth={2,m_gBuffers->getDepthImage(),m_gBuffers->getDepthImageView(),m_gBuffers->getDepthFormat(),VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_IMAGE_ASPECT_DEPTH_BIT,m_graphicsQueueIndex,f.renderExtent};
    inputs.motionVectors={3,m_gBuffers->getColorImage(1),m_gBuffers->getColorImageView(1),m_gBuffers->getColorFormat(1),VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_ASPECT_COLOR_BIT,m_graphicsQueueIndex,f.renderExtent};
    return inputs;
  }

  void renderOutput(VkCommandBuffer cmd,bool generated)
  {
    const VkExtent2D outputSize=getSize();
    // Post-processing
    {
      const auto swapIndex=m_swapChain.getActiveImageIndex();
      const auto oldLayout=useManagedSwapchain()&&!directSwapSeen[swapIndex]?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      nvvk::cmdBarrierImageLayout(cmd,m_swapChain.getActiveImage(),oldLayout,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      if(useManagedSwapchain())directSwapSeen[swapIndex]=true;

      const nvvk::createRenderingInfo renderingInfo({{0, 0}, getSize()}, {m_swapChain.getActiveImageView()}, VK_NULL_HANDLE,
                                                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
      vkCmdBeginRendering(cmd, &renderingInfo);

      const VkViewport viewport = {
          0.0f, 0.0f, static_cast<float>(outputSize.width), static_cast<float>(outputSize.height), 0.0f, 1.0f};
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      const VkRect2D scissor = {{0, 0}, {outputSize.width, outputSize.height}};
      vkCmdSetScissor(cmd, 0, 1, &scissor);

      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_postPipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_dset->getPipeLayout(), 0, 1,
                              m_dset->getSets()+(generated?1:0), 0, nullptr);

      vkCmdDraw(cmd, 3, 1, 0, 0);

      vkCmdEndRendering(cmd);
    }

    // ImGui rendering
    if(ImDrawData* const drawData = ImGui::GetDrawData();
       drawData != nullptr && drawData->TotalVtxCount != 0 && drawData->TotalIdxCount != 0)
    {
      const nvvk::createRenderingInfo renderingInfo({{0, 0}, getSize()}, {m_swapChain.getActiveImageView()}, VK_NULL_HANDLE,
                                                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
      vkCmdBeginRendering(cmd, &renderingInfo);

      ImGui_ImplVulkan_RenderDrawData(drawData, cmd);

      vkCmdEndRendering(cmd);
    }

    nvvk::cmdBarrierImageLayout(cmd, m_swapChain.getActiveImage(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

  }

  void submitFrame() override
  {
    if(g_srOnly){
      const uint32_t index=m_swapChain.getActiveImageIndex();
      NVVK_CHECK(vkResetFences(m_device,1,&m_waitFences[index]));
      const VkSemaphore acquired=m_swapChain.getActiveReadSemaphore();
      const VkSemaphore ready=srPresentReady[index];
      constexpr VkPipelineStageFlags stage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.waitSemaphoreCount=submit.signalSemaphoreCount=submit.commandBufferCount=1;
      submit.pWaitSemaphores=&acquired;
      submit.pWaitDstStageMask=&stage;
      submit.pSignalSemaphores=&ready;
      submit.pCommandBuffers=&m_commandBuffers[index];
      NVVK_CHECK(vkQueueSubmit(m_queue,1,&submit,m_waitFences[index]));
      VkPresentInfoKHR present{};
      m_swapChain.presentCustom(present);
      present.pWaitSemaphores=&ready;
      const VkResult result=vkQueuePresentKHR(m_queue,&present);
      if(result!=VK_SUCCESS && result!=VK_SUBOPTIMAL_KHR) {
        ++srErrors;
        std::printf("SR_PRESENT_ERROR=%d\n",int(result));
      }
      return;
    }
    if(!g_directFG){AppBaseVk::submitFrame();return;}
    auto submitPresent=[&](bool generated){
      uint32_t i=m_swapChain.getActiveImageIndex();NVVK_CHECK(vkResetFences(m_device,1,&m_waitFences[i]));
      VkSemaphore read=m_swapChain.getActiveReadSemaphore(),write=m_swapChain.getActiveWrittenSemaphore();
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
      // Wait before the first swapchain layout transition in the post command buffer.
      VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
#else
      VkPipelineStageFlags stage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
#endif
      VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.waitSemaphoreCount=si.signalSemaphoreCount=si.commandBufferCount=1;si.pWaitSemaphores=&read;si.pWaitDstStageMask=&stage;si.pSignalSemaphores=&write;si.pCommandBuffers=&m_commandBuffers[i];
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
      if(sdkConsumerCommands)si.pCommandBuffers=&sdkConsumerCommands;
      NVVK_CHECK(directFG.submit(si,m_waitFences[i]));
#else
      NVVK_CHECK(vkQueueSubmit(m_queue,1,&si,m_waitFences[i]));
#endif
      VkPresentInfoKHR present{};m_swapChain.presentCustom(present);auto r=vkQueuePresentKHR(m_queue,&present);
      if(r==VK_SUCCESS||r==VK_SUBOPTIMAL_KHR){if(generated)++directGeneratedPresents;else ++directRealPresents;}
      else {++directFG.errors;directFG.enabled=false;printf("DIRECT_PRESENT_ERROR=%d\n",int(r));}
      NVVK_CHECK(vkQueueWaitIdle(m_queue));
    };
    submitPresent(directReady);
    directFG.onGpuCompleted();
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
    if(sdkConsumerCommands){
      vkFreeCommandBuffers(m_device,m_cmdPool,1,&sdkConsumerCommands);
      sdkConsumerCommands=VK_NULL_HANDLE;
    }
#endif
    if(directReady){
      bool recreated=false;
      if(!m_swapChain.acquire(&recreated)||recreated){
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
        directFG.finishFrame();
#endif
        directFG.enabled=false;++directFG.errors;return;
      }
      const auto i=m_swapChain.getActiveImageIndex();NVVK_CHECK(vkWaitForFences(m_device,1,&m_waitFences[i],VK_TRUE,30000000000ull));
      VkCommandBuffer cmd=m_commandBuffers[i];NVVK_CHECK(vkResetCommandBuffer(cmd,0));VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      NVVK_CHECK(vkBeginCommandBuffer(cmd,&bi));renderOutput(cmd,false);NVVK_CHECK(vkEndCommandBuffer(cmd));submitPresent(false);
    }
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
    directFG.finishFrame();
#endif
  }

  void drawGui()
  {
    ImGui::SetNextWindowSize(ImVec2(320, 540), ImGuiCond_FirstUseEver);
    ImGui::Begin("Settings");

    // The framerate calculated by the application does not account for the additional present calls introduced by DLSS Frame Generation, so need to adjust it accordingly
    uint32_t numFramesActuallyPresented = 1;
    if(m_dlssgSupported == sl::Result::eOk)
    {
      sl::DLSSGState dlssgState;
      if(SL_FAILED(res, slDLSSGGetState(m_viewportHandle, dlssgState, &m_dlssgOptions)))  // Only call this once per frame!
      {
        ++fgErrors;
        LOGE("Streamline: Failed to get DLSS Frame Generation state (%d)\n", res);
      }
      else if(dlssgState.status != sl::DLSSGStatus::eOk)
      {
        ++fgErrors;
        LOGE("Streamline: DLSS Frame Generation status is %u\n", dlssgState.status);

        // Turn off DLSS Frame Generation if something went wrong
        m_dlssgOptions.mode = sl::DLSSGMode::eOff;

        if(!g_directFG) slDLSSGSetOptions(m_viewportHandle, m_dlssgOptions);
      }

      numFramesActuallyPresented = std::max(dlssgState.numFramesActuallyPresented, 1u);
      if(g_verifyFG) {
        fgPresented += numFramesActuallyPresented - 1;
        std::printf("FG_STATE presented=%u status=%u estimate_vram=%llu\n", dlssgState.numFramesActuallyPresented, unsigned(dlssgState.status), static_cast<unsigned long long>(dlssgState.estimatedVRAMUsageInBytes));
      }
    }
    ImGui::Text("Real FPS: %.1f",ImGui::GetIO().Framerate);
    if(g_directFG){
      bool vsync=directSwapchainDirty?directRequestedVsync:m_vsync;
      if(ImGui::Checkbox("VSync (V)",&vsync))requestVsync(vsync);
      ImGui::Text("Presentation: %s",directPresentModeName());
      if(!vsync&&directPresentMode==VK_PRESENT_MODE_FIFO_KHR&&!directSwapchainDirty)
        ImGui::TextWrapped("This display exposes FIFO only.");
    }
    if(g_directFG){ImGui::TextWrapped("NVIDIA DLSS FG 2X / Direct NGX");ImGui::TextWrapped("%s",directFG.status);ImGui::TextWrapped("Presented real / generated: %u / %u",directRealPresents,directGeneratedPresents);ImGui::Text("FG errors: %u",directFG.errors);}

    if(ImGui::CollapsingHeader("Reflex"))
    {
      ImGui::PushID("Reflex");

      bool modified = false;

      modified |= ImGui::Combo("Mode", reinterpret_cast<int*>(&m_reflexOptions.mode), "Off\0Low Latency\0Low Latency with Boost\0");

      if(int fps = m_reflexOptions.frameLimitUs != 0 ? static_cast<int>((1000.0f / m_reflexOptions.frameLimitUs) * 1000.0f) : 0;
         ImGui::InputInt("FPS Limit", &fps, 1, 100, ImGuiInputTextFlags_EnterReturnsTrue))
      {
        // Clamp possible FPS range
        if(fps < 0)
          fps = 0;
        if(fps > 200)
          fps = 200;

        m_reflexOptions.frameLimitUs = static_cast<uint32_t>((1000.0f / fps) * 1000.0f);
        modified                     = true;
      }

      if(modified)
      {
        if(SL_FAILED(res, slReflexSetOptions(m_reflexOptions)))
        {
          LOGE("Streamline: Failed to set Reflex options (%d)\n", res);
        }
      }

      ImGui::PopID();
    }

    if(!g_directFG && ImGui::CollapsingHeader("DLSS Super Resolution"))
    {
      ImGui::PushID("DLSS Super Resolution");

      if(m_dlssSupported == sl::Result::eOk)
      {
        bool modified = false;

        modified |= ImGui::Combo("Mode", reinterpret_cast<int*>(&m_dlssOptions.mode),
                                 "Off\0Performance\0Balanced\0Quality\0Ultra Performance\0Ultra Quality\0DLAA\0");
        modified |= ImGui::SliderFloat("Sharpness", &m_dlssOptions.sharpness, 0.0f, 1.0f);

        if(modified)
        {
          createImages(getSize());
        }
      }
      else
      {
        ImGui::TextWrapped("DLSS Super Resolution is not supported on this adapter (return code %d)", m_dlssSupported);
      }

      ImGui::PopID();
    }

    if(ImGui::CollapsingHeader("DLSS Frame Generation"))
    {
      ImGui::PushID("DLSS Frame Generation");

      if(g_directFG)
      {
        if(ImGui::Checkbox("Enabled",&directFG.enabled))directFG.reset=true;
        ImGui::TextWrapped("Direct NGX uses this scene's color, depth and motion vectors. SR is off in this mode.");
      }
      else if(m_dlssgSupported == sl::Result::eOk)
      {
        bool modified = false;

        modified |= ImGui::Checkbox("Enabled", reinterpret_cast<bool*>(&m_dlssgOptions.mode));

        if(modified)
        {
          // DLSS Frame Generation requires Reflex to be on
          if(m_dlssgOptions.mode != sl::DLSSGMode::eOff && m_reflexOptions.mode == sl::ReflexMode::eOff)
          {
            m_reflexOptions.mode = sl::ReflexMode::eLowLatency;

            slReflexSetOptions(m_reflexOptions);
          }

          if(SL_FAILED(res, slDLSSGSetOptions(m_viewportHandle, m_dlssgOptions)))
          {
            LOGE("Streamline: Failed to set DLSS Frame Generation options (%d)\n", res);
          }
        }
      }
      else
      {
        if(m_dlssgSupported == sl::Result::eErrorOSDisabledHWS)
        {
          ImGui::TextWrapped(
              "DLSS Frame Generation is not supported because hardware-accelerated GPU scheduling is not enabled: "
              "https://devblogs.microsoft.com/directx/hardware-accelerated-gpu-scheduling/");
        }
        else
        {
          ImGui::TextWrapped("DLSS Frame Generation is not supported on this adapter (return code %d)", m_dlssgSupported);
        }
      }

      ImGui::PopID();
    }

    ImGui::End();
  }

private:
  void createScene()
  {
    // Create meshes
    m_sceneMeshes.emplace_back(nvh::createSphereUv());
    m_sceneMeshes.emplace_back(nvh::createCube());
    m_sceneMeshes.emplace_back(nvh::createTetrahedron());
    m_sceneMeshes.emplace_back(nvh::createOctahedron());
    m_sceneMeshes.emplace_back(nvh::createIcosahedron());
    m_sceneMeshes.emplace_back(nvh::createConeMesh());
    const int num_meshes = static_cast<int>(m_sceneMeshes.size());

    const VkCommandBuffer cmd = createTempCmdBuffer();

    for(int i = 0; i < num_meshes; i++)
    {
      PrimitiveMeshVk& mesh = m_sceneMeshes[i];
      mesh.verticesBuffer   = m_alloc->createBuffer(cmd, mesh.vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
      mesh.trianglesBuffer  = m_alloc->createBuffer(cmd, mesh.triangles, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    }

    m_frameInfo = m_alloc->createBuffer(sizeof(FrameInfo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    submitTempCmdBuffer(cmd);

    // Create instances/nodes
    for(int i = 0; i < num_meshes; i++)
    {
      Node& node       = m_sceneNodes.emplace_back();
      node.mesh        = i;
      node.material    = i;
      node.translation = glm::vec3(-(static_cast<float>(num_meshes) * 0.5f) + static_cast<float>(i), 0.0f, 0.0f);
      node.motion      = true;
    }

    Node& background       = m_sceneNodes.emplace_back();
    background.mesh        = 1;
    background.translation = {0.0f, 0.0f, -5.0f};
    background.scale       = {50, 50, 50};

    CameraManip.setClipPlanes({0.1f, 100.0f});
    CameraManip.setLookat({0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
  }

  void createPipelines()
  {
    m_dset->addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    m_dset->addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

    m_dset->initLayout();
    m_dset->initPool(g_directFG?2:1);
    const VkPushConstantRange pushConstantRange = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(NodeInfo)};
    m_dset->initPipeLayout(1, &pushConstantRange);

    // Update descriptor set
    {
      const VkDescriptorBufferInfo frameInfoBufferInfo = {m_frameInfo.buffer, 0, VK_WHOLE_SIZE};
      const VkWriteDescriptorSet   frameInfoWrite      = m_dset->makeWrite(0, 0, &frameInfoBufferInfo);
      vkUpdateDescriptorSets(m_device, 1, &frameInfoWrite, 0, nullptr);
      if(g_directFG){auto second=m_dset->makeWrite(1,0,&frameInfoBufferInfo);vkUpdateDescriptorSets(m_device,1,&second,0,nullptr);}
    }

    // Create the scene rendering pipeline
    {
      nvvk::GraphicsPipelineState pipelineState;
      pipelineState.rasterizationState.cullMode = VK_CULL_MODE_NONE;
      pipelineState.addBindingDescriptions({{0, sizeof(nvh::PrimitiveVertex)}});
      pipelineState.addAttributeDescriptions({
          {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(nvh::PrimitiveVertex, p))},  // Position
          {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(nvh::PrimitiveVertex, n))},  // Normal
      });

      pipelineState.clearBlendAttachmentStates();
      for(uint32_t i = 0; i < std::size(SAMPLE_COLOR_FORMATS); i++)
      {
        pipelineState.addBlendAttachmentState(nvvk::GraphicsPipelineState::makePipelineColorBlendAttachmentState());
      }

      VkPipelineRenderingCreateInfo renderingInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
      renderingInfo.colorAttachmentCount          = static_cast<uint32_t>(std::size(SAMPLE_COLOR_FORMATS));
      renderingInfo.pColorAttachmentFormats       = SAMPLE_COLOR_FORMATS;
      renderingInfo.depthAttachmentFormat         = SAMPLE_DEPTH_FORMAT;

      nvvk::GraphicsPipelineGenerator pipelineGenerator(m_device, m_dset->getPipeLayout(), renderingInfo, pipelineState);
      pipelineGenerator.addShader(std::vector<uint32_t>{std::begin(scene_vert), std::end(scene_vert)}, VK_SHADER_STAGE_VERTEX_BIT);
      pipelineGenerator.addShader(std::vector<uint32_t>{std::begin(scene_frag), std::end(scene_frag)}, VK_SHADER_STAGE_FRAGMENT_BIT);

      m_scenePipeline = pipelineGenerator.createPipeline();
    }

    // Create the post-processing pipeline
    {
      nvvk::GraphicsPipelineState pipelineState;
      pipelineState.rasterizationState.cullMode = VK_CULL_MODE_NONE;

      const VkFormat colorFormat = m_swapChain.getFormat();

      VkPipelineRenderingCreateInfo renderingInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
      renderingInfo.colorAttachmentCount          = 1;
      renderingInfo.pColorAttachmentFormats       = &colorFormat;

      nvvk::GraphicsPipelineGenerator pipelineGenerator(m_device, m_dset->getPipeLayout(), renderingInfo, pipelineState);
      pipelineGenerator.addShader(std::vector<uint32_t>{std::begin(post_vert), std::end(post_vert)}, VK_SHADER_STAGE_VERTEX_BIT);
      pipelineGenerator.addShader(std::vector<uint32_t>{std::begin(post_frag), std::end(post_frag)}, VK_SHADER_STAGE_FRAGMENT_BIT);

      m_postPipeline = pipelineGenerator.createPipeline();
    }
  }

  void createImages(const VkExtent2D& size)
  {
    vkDeviceWaitIdle(m_device);

    destroyImages();

    // Update output dimensions for DLSS Super Resolution
    m_dlssOptions.outputWidth  = size.width;
    m_dlssOptions.outputHeight = size.height;
    m_dlssOptions.colorBuffersHDR =
        SAMPLE_COLOR_FORMATS[0] >= VK_FORMAT_A2R10G10B10_UNORM_PACK32 ? sl::Boolean::eTrue : sl::Boolean::eFalse;

    if(SL_FAILED(res, slDLSSSetOptions(m_viewportHandle, m_dlssOptions)))
    {
      LOGE("Streamline: Failed to set DLSS Super Resolution options (%d)\n", res);
    }

    sl::DLSSOptimalSettings dlssSettings;
    if(SL_FAILED(res, slDLSSGetOptimalSettings(m_dlssOptions, dlssSettings)) || m_dlssOptions.mode == sl::DLSSMode::eOff
       || dlssSettings.optimalRenderWidth == 0 || dlssSettings.optimalRenderHeight == 0)
    {
      // Default render dimensions to the window dimensions if DLSS is not enabled
      dlssSettings.optimalRenderWidth  = size.width;
      dlssSettings.optimalRenderHeight = size.height;
    }

    const VkExtent2D renderSize = {dlssSettings.optimalRenderWidth, dlssSettings.optimalRenderHeight};

    m_gBuffers = std::make_shared<nvvkhl::GBuffer>(
        m_device, m_alloc.get(), renderSize,
        std::vector<VkFormat>(std::begin(SAMPLE_COLOR_FORMATS), std::end(SAMPLE_COLOR_FORMATS)), SAMPLE_DEPTH_FORMAT);

    if(m_dlssOptions.mode != sl::DLSSMode::eOff)
    {
      // The image DLSS Super Resolution outputs to must have the VK_IMAGE_USAGE_STORAGE_BIT usage flag, or else nothing will be rendered to it!
      const VkImageCreateInfo outputCreateInfo =
          nvvk::makeImage2DCreateInfo(size, SAMPLE_COLOR_FORMATS[0], VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
      m_outputImage = m_alloc->createImage(outputCreateInfo);

      const VkImageViewCreateInfo outputViewCreateInfo =
          nvvk::makeImage2DViewCreateInfo(m_outputImage.image, outputCreateInfo.format);
      NVVK_CHECK(vkCreateImageView(m_device, &outputViewCreateInfo, nullptr, &m_outputImageView));
    }
    else
    {
      m_outputImage.image = m_gBuffers->getColorImage();
      m_outputImageView   = m_gBuffers->getColorImageView();
    }

    m_frameOwner=std::make_shared<SampleFrameOwner>(SampleFrameOwner{m_alloc,m_gBuffers});
    ++m_inputGeneration;
    if(g_directFG){
      auto cmd=createTempCmdBuffer();directFG.resize(cmd,size,g_verifyDirectFG);
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
      if(g_cancelFirstFgResize) {
        g_cancelFirstFgResize=false;
        // Discard the native recording before releasing the objects it references.
        vkFreeCommandBuffers(m_device,m_cmdPool,1,&cmd);
        if(!directFG.cancelUnsubmitted(false))
          throw std::runtime_error("native resize cancellation did not exercise a recording");
        std::puts("PUBLIC_VULKAN_FG native_resize_cancelled=1");
        cmd=createTempCmdBuffer();
        directFG.resize(cmd,size,g_verifyDirectFG);
      }
      NVVK_CHECK(vkEndCommandBuffer(cmd));
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;
      NVVK_CHECK(directFG.submit(submit,VK_NULL_HANDLE));
      NVVK_CHECK(vkQueueWaitIdle(m_queue));
      directFG.onGpuCompleted();
      vkFreeCommandBuffers(m_device,m_cmdPool,1,&cmd);
#else
      submitTempCmdBuffer(cmd);
#endif
      if(directFG.outputView){VkDescriptorImageInfo image={m_defaultSampler,directFG.outputView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};auto write=m_dset->makeWrite(1,1,&image);vkUpdateDescriptorSets(m_device,1,&write,0,nullptr);}
    }

    // Update descriptor set with the new output image
    {
      const VkDescriptorImageInfo outputImageInfo = {m_defaultSampler, m_outputImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      const VkWriteDescriptorSet outputImageWrite = m_dset->makeWrite(0, 1, &outputImageInfo);
      vkUpdateDescriptorSets(m_device, 1, &outputImageWrite, 0, nullptr);
    }
  }

  void destroyScene()
  {
    for(PrimitiveMeshVk& mesh : m_sceneMeshes)
    {
      m_alloc->destroy(mesh.verticesBuffer);
      m_alloc->destroy(mesh.trianglesBuffer);
    }

    m_sceneNodes.clear();
    m_sceneMeshes.clear();

    m_alloc->destroy(m_frameInfo);
  }

  void destroyPipelines()
  {
    vkDestroyPipeline(m_device, m_scenePipeline, nullptr);
    m_scenePipeline = VK_NULL_HANDLE;

    vkDestroyPipeline(m_device, m_postPipeline, nullptr);
    m_postPipeline = VK_NULL_HANDLE;
  }

  void destroyImages()
  {
    if(m_outputImage.memHandle != nullptr)
    {
      vkDestroyImageView(m_device, m_outputImageView, nullptr);

      m_alloc->destroy(m_outputImage);
    }
    else
    {
      m_outputImage.image = VK_NULL_HANDLE;
    }

    m_outputImageView = VK_NULL_HANDLE;

    m_frameOwner.reset();
    m_gBuffers.reset();
  }

  sl::Result m_dlssSupported  = sl::Result::eOk;
  sl::Result m_dlssgSupported = sl::Result::eOk;

  sl::DLSSOptions   m_dlssOptions;
  sl::DLSSGOptions  m_dlssgOptions;
  sl::ReflexOptions m_reflexOptions;

  sl::ViewportHandle m_viewportHandle;

  std::unique_ptr<nvvk::DescriptorSetContainer> m_dset;
  std::shared_ptr<nvvk::ResourceAllocator>      m_alloc;
  VkSampler                                     m_defaultSampler = VK_NULL_HANDLE;

  struct PrimitiveMeshVk : nvh::PrimitiveMesh
  {
    PrimitiveMeshVk(const nvh::PrimitiveMesh& mesh)
        : PrimitiveMesh(mesh)
    {
    }

    nvvk::Buffer verticesBuffer;
    nvvk::Buffer trianglesBuffer;
  };
  std::vector<PrimitiveMeshVk> m_sceneMeshes;
  struct Node : nvh::Node
  {
    bool motion = false;
  };
  std::vector<Node> m_sceneNodes;
  nvvk::Buffer      m_frameInfo;

  VkPipeline m_scenePipeline = VK_NULL_HANDLE;
  VkPipeline m_postPipeline  = VK_NULL_HANDLE;

  struct SampleFrameOwner {
    // Reverse destruction order: destroy actual views/images before their allocator.
    std::shared_ptr<nvvk::ResourceAllocator> allocator;
    std::shared_ptr<nvvkhl::GBuffer> buffers;
  };
  std::shared_ptr<SampleFrameOwner> m_frameOwner;
  std::uint64_t m_inputGeneration=0,m_sourceFrameIndex=0;
  std::shared_ptr<nvvkhl::GBuffer> m_gBuffers;
  nvvk::Image                      m_outputImage;
  VkImageView                      m_outputImageView = VK_NULL_HANDLE;

  float     m_animationTime     = 0.0f;
  float     m_animationTimePrev = 0.0f;
  glm::mat4 m_projMatrixPrev{1.f};
  glm::mat4 m_viewMatrixPrev{1.f};
};

int main(int argc, char** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  for(int i=1;i<argc;++i){
    if(!strcmp(argv[i],"--streamline")||!strcmp(argv[i],"--verify-sr")||!strcmp(argv[i],"--verify-fg"))g_directFG=false;
    if(!strcmp(argv[i],"--verify-fg"))g_verifyFG=true;
    if(!strcmp(argv[i],"--verify-sr"))g_srOnly=true;
    if(!strcmp(argv[i],"--validation"))g_validation=true;
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
    if(!strcmp(argv[i],"--sdk-handoff-same"))g_sdkHandoff=SdkHandoff::sameFamily;
    if(!strcmp(argv[i],"--sdk-handoff-cross"))g_sdkHandoff=SdkHandoff::crossFamily;
    if(!strcmp(argv[i],"--sdk-cancel-first-resize"))g_cancelFirstFgResize=true;
    if(!strcmp(argv[i],"--sdk-cancel-evaluate-frame")&&i+1<argc)g_cancelEvaluateFrame=std::max(0,atoi(argv[++i]));
#endif
    if(!strcmp(argv[i],"--test-missing-depth-frame")&&i+1<argc)g_missingDepthFrame=std::max(0,atoi(argv[++i]));
    if(!strcmp(argv[i],"--no-vsync"))g_initialVsync=false;
    if(!strcmp(argv[i],"--vsync"))g_initialVsync=true;
    if(!strcmp(argv[i],"--verify-direct-fg")){g_verifyDirectFG=true;g_frameLimit=24;}
    if(!strcmp(argv[i],"--frames")&&i+1<argc)g_frameLimit=std::max(2,atoi(argv[++i]));
  }
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
  if(g_cancelEvaluateFrame>=0&&(!g_directFG||g_sdkHandoff!=SdkHandoff::sameFamily||g_missingDepthFrame==g_cancelEvaluateFrame)) {
    std::fputs("Evaluate cancellation requires same-family installed FG and an accepted input frame\n",stderr);
    return 2;
  }
#endif
  g_srOnly=g_srOnly&&!g_verifyFG;
  const bool verifySr=g_srOnly||g_verifyFG;
  try {
    configureRuntime();
  } catch(const std::exception& error) {
    std::fprintf(stderr,"FrameLab runtime configuration: %s\n",error.what());
    return 2;
  }
  if(g_directFG || verifySr) { SAMPLE_WIDTH = 1280; SAMPLE_HEIGHT = 720; }
  unsigned verifiedFrames = 0;
  auto seriesStart=std::chrono::steady_clock::now();
  // Setup GLFW window
  if(!glfwInit())
  {
    LOGE("GLFW: Initialization failed\n");
    return static_cast<int>(sl::Result::eErrorNotInitialized);
  }

  if(!glfwVulkanSupported())
  {
    LOGE("GLFW: Vulkan not supported\n");
    return static_cast<int>(sl::Result::eErrorAdapterNotSupported);
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow* const window = glfwCreateWindow(SAMPLE_WIDTH, SAMPLE_HEIGHT, PROJECT_NAME, nullptr, nullptr);
  if(window == nullptr)
  {
    LOGE("GLFW: Failed to create window\n");
    return static_cast<int>(sl::Result::eErrorNotInitialized);
  }

  std::vector<sl::Feature> slFeatures(std::begin(SL_FEATURES),std::end(SL_FEATURES));
  if(useManagedSwapchain())slFeatures.pop_back(); // Only the legacy Streamline mode requests Streamline FG.
  // Initialize Streamline (this must happen before any Vulkan calls are made)
  sl::Preferences pref;
  pref.showConsole = false;
  pref.flags = sl::PreferenceFlags::eDisableCLStateTracking; // Pin SDK DLLs: no OTA plugins.
  pref.pathToLogsAndData = g_streamlineLogDir.c_str();
  pref.logLevel    = sl::LogLevel::eVerbose;
#if SL_MANUAL_HOOKING
  pref.flags |= sl::PreferenceFlags::eUseManualHooking;
#endif
  pref.featuresToLoad    = slFeatures.data();
  pref.numFeaturesToLoad = static_cast<uint32_t>(slFeatures.size());
  pref.applicationId     = 231313132;
  pref.engine            = sl::EngineType::eCustom;
  pref.engineVersion     = 0;
  pref.renderAPI         = sl::RenderAPI::eVulkan;

  if(SL_FAILED(res, slInit(pref)))
  {
    LOGE("Streamline: Initialization failed (%d)\n", res);
    return static_cast<int>(res);
  }

  nvvk::ContextCreateInfo contextInfo(/* bUseValidation = */ false);
  contextInfo.setVersion(1, 3);
  if(g_validation) {
    contextInfo.addInstanceLayer("VK_LAYER_KHRONOS_validation");
    contextInfo.addInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }

  // Add Vulkan extensions required by GLFW
  {
    contextInfo.addDeviceExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    uint32_t                 vkNumInstanceExtensions = 0;
    const char* const* const vkInstanceExtensions    = glfwGetRequiredInstanceExtensions(&vkNumInstanceExtensions);
    for(uint32_t i = 0; i < vkNumInstanceExtensions; i++)
      contextInfo.addInstanceExtension(vkInstanceExtensions[i]);
  }

  bool useVsync = useManagedSwapchain()&&g_initialVsync;
  if(g_directFG){for(const char* ext:{"VK_NVX_binary_import","VK_NVX_image_view_handle","VK_KHR_push_descriptor","VK_KHR_external_memory_win32","VK_KHR_external_semaphore_win32"})contextInfo.addDeviceExtension(ext);}


#if SL_MANUAL_HOOKING
  VkPhysicalDeviceOpticalFlowFeaturesNV opticalFlowFeatures = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV};

  // Add Vulkan extensions required by Streamline features
  for(const sl::Feature feature : slFeatures)
  {
    sl::FeatureRequirements requirements;
    if(SL_FAILED(res, slGetFeatureRequirements(feature, requirements)))
    {
      LOGE("Streamline: Failed to get feature requirements for feature %u (%d)\n", feature, res);
      return static_cast<int>(res);
    }
    if((requirements.flags & sl::FeatureRequirementFlags::eVulkanSupported) == 0)
    {
      LOGE("Streamline: Feature %u is not supported on Vulkan\n", feature);
      return static_cast<int>(sl::Result::eErrorFeatureNotSupported);
    }

    if((requirements.flags & sl::FeatureRequirementFlags::eVSyncOffRequired) != 0)
      useVsync = false;

    if(requirements.vkNumComputeQueuesRequired != 0)
      contextInfo.addRequestedQueue(VK_QUEUE_COMPUTE_BIT, requirements.vkNumComputeQueuesRequired);
    if(requirements.vkNumGraphicsQueuesRequired != 0)
      contextInfo.addRequestedQueue(VK_QUEUE_GRAPHICS_BIT, requirements.vkNumGraphicsQueuesRequired);
    if(requirements.vkNumOpticalFlowQueuesRequired != 0)
      contextInfo.addRequestedQueue(VK_QUEUE_OPTICAL_FLOW_BIT_NV, requirements.vkNumOpticalFlowQueuesRequired);

    for(uint32_t i = 0; i < requirements.vkNumDeviceExtensions; i++)
    {
      void* featureStruct = nullptr;
      if(strcmp(requirements.vkDeviceExtensions[i], VK_NV_OPTICAL_FLOW_EXTENSION_NAME) == 0)
      {
        featureStruct                   = &opticalFlowFeatures;
        opticalFlowFeatures.opticalFlow = VK_TRUE;
      }

      contextInfo.addDeviceExtension(requirements.vkDeviceExtensions[i], false, featureStruct);
    }
    for(uint32_t i = 0; i < requirements.vkNumInstanceExtensions; i++)
    {
      contextInfo.addInstanceExtension(requirements.vkInstanceExtensions[i]);
    }
  }
#endif

  // Create Vulkan device and queues
  VkPhysicalDevicePresentIdFeaturesKHR directPresentId{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
  if(useManagedSwapchain()) {
    // The core Vulkan 1.2 bufferDeviceAddress feature supersedes the legacy EXT.
    contextInfo.removeDeviceExtension(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    // Reflex's VK_NV_low_latency2 dependency needs present_id or present_id2.
    contextInfo.addDeviceExtension(VK_KHR_PRESENT_ID_EXTENSION_NAME,false,&directPresentId);
  }
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
  auto contextOwner=std::shared_ptr<nvvk::Context>(new nvvk::Context,[](auto* context){
    context->deinit();delete context;
  });
  auto& context=*contextOwner;
#else
  nvvk::Context context;
#endif
  if(!context.initInstance(contextInfo))
  {
    return static_cast<int>(sl::Result::eErrorVulkanAPI);
  }

  VkDebugUtilsMessengerEXT validationMessenger{};
  if(g_validation){
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;debug.pfnUserCallback=sampleValidation;
    auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(context.m_instance,"vkCreateDebugUtilsMessengerEXT"));
    if(!create||create(context.m_instance,&debug,nullptr,&validationMessenger)!=VK_SUCCESS)return 2;
    printf("SAMPLE_VALIDATION_MESSENGER_CREATED\n");
  }
  const auto compatible=context.getCompatibleDevices(contextInfo);
  if(compatible.empty())return static_cast<int>(sl::Result::eErrorVulkanAPI);
  const auto deviceIndex=compatible[contextInfo.compatibleDeviceIndex];
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
  bool requestSameFamilyProducer=false;
  if(g_directFG&&g_sdkHandoff==SdkHandoff::sameFamily) {
    const auto physical=context.getPhysicalDevices().at(deviceIndex);
    uint32_t count=0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
    const auto required=contextInfo.defaultQueueGCT;
    requestSameFamilyProducer=std::any_of(families.begin(),families.end(),[required](const auto& family) {
      return family.queueCount>=2&&(family.queueFlags&required)==required;
    });
    if(requestSameFamilyProducer)contextInfo.addRequestedQueue(required);
  }
#endif
  if(!context.initDevice(deviceIndex,contextInfo))return static_cast<int>(sl::Result::eErrorVulkanAPI);
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
  // Reserve the requested producer before the optional Streamline queue lookups
  // consume entries from the same nvpro queue pool.
  nvvk::Context::Queue sdkProducer{};
  if(requestSameFamilyProducer)
    sdkProducer=context.createQueue(contextInfo.defaultQueueGCT,"FrameLab graphics producer");
  if(g_directFG&&g_sdkHandoff==SdkHandoff::crossFamily)
    sdkProducer=context.m_queueT;
#endif
  // Verify that the requested features are actually supported on the created device
  sl::AdapterInfo adapter;
  adapter.vkPhysicalDevice = context.m_physicalDevice;
  // This sample can optionally run without DLSS, but it requires Reflex
  sl::Result dlssSupported  = sl::Result::eOk;
  sl::Result dlssgSupported = useManagedSwapchain()?sl::Result::eErrorFeatureNotSupported:sl::Result::eOk;

  for(const sl::Feature feature : slFeatures)
  {
    if(SL_FAILED(res, slIsFeatureSupported(feature, adapter)))
    {
      if(res == sl::Result::eErrorOSDisabledHWS)
      {
        LOGW("Streamline: Feature %u is not supported because hardware-accelerated GPU scheduling is not enabled: https://devblogs.microsoft.com/directx/hardware-accelerated-gpu-scheduling/.\n",
             feature);
      }
      else
      {
        LOGE(
            "Streamline: Feature %u is not supported on this adapter (return code %d). "
            "The log messages from Streamline may include more information.\n",
            feature, res);
      }

      switch(feature)
      {
        case sl::kFeatureDLSS:
          LOGW("This sample can still run, but DLSS Super Resolution will not be available\n");
          dlssSupported = res;
          break;
        case sl::kFeatureDLSS_G:
          LOGW("This sample can still run, but DLSS Frame Generation will not be available\n");
          dlssgSupported = res;
          break;
        default:
          return static_cast<int>(res);
      }
    }
  }

#if SL_MANUAL_HOOKING
  StreamlineWrapper::get().initVulkanHooks(context.m_device);

  // Inform Streamline about the main Vulkan device and queues
  sl::VulkanInfo vulkanInfo;
  vulkanInfo.device         = context.m_device;
  vulkanInfo.instance       = context.m_instance;
  vulkanInfo.physicalDevice = context.m_physicalDevice;

  if(const nvvk::Context::Queue queueC = context.createQueue(VK_QUEUE_COMPUTE_BIT, "queueC"); queueC.queue != VK_NULL_HANDLE)
  {
    vulkanInfo.computeQueueIndex  = queueC.queueIndex;
    vulkanInfo.computeQueueFamily = queueC.familyIndex;
  }
  if(const nvvk::Context::Queue queueG = context.createQueue(VK_QUEUE_GRAPHICS_BIT, "queueG"); queueG.queue != VK_NULL_HANDLE)
  {
    vulkanInfo.graphicsQueueIndex  = queueG.queueIndex;
    vulkanInfo.graphicsQueueFamily = queueG.familyIndex;
  }
  if(const nvvk::Context::Queue queueOF = context.createQueue(VK_QUEUE_OPTICAL_FLOW_BIT_NV, "queueOF"); queueOF.queue != VK_NULL_HANDLE)
  {
    vulkanInfo.opticalFlowQueueIndex    = queueOF.queueIndex;
    vulkanInfo.opticalFlowQueueFamily   = queueOF.familyIndex;
    vulkanInfo.useNativeOpticalFlowMode = true;
  }

  if(SL_FAILED(res, slSetVulkanInfo(vulkanInfo)))
  {
    LOGE("Streamline: Failed to set Vulkan info (%d)\n", res);
    return static_cast<int>(res);
  }
#endif

  // Create window surface
  VkSurfaceKHR surface = VK_NULL_HANDLE;
#if 0
  const VkResult surfaceResult = glfwCreateWindowSurface(context.m_instance, window, nullptr, &surface);
#else
  VkWin32SurfaceCreateInfoKHR surfaceInfo = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&glfwGetWin32Window), &surfaceInfo.hinstance);
  surfaceInfo.hwnd = glfwGetWin32Window(window);

  // Call vkCreateWin32SurfaceKHR directly so that it is routed through Streamline
  const VkResult surfaceResult = vkCreateWin32SurfaceKHR(context.m_instance, &surfaceInfo, nullptr, &surface);
#endif
  if(VK_SUCCESS != surfaceResult)
  {
    LOGE("Creating the window surface failed with code %s!\n", string_VkResult(surfaceResult));
    return static_cast<int>(sl::Result::eErrorVulkanAPI);
  }

  // Create main application
  nvvkhl::AppBaseVkCreateInfo appInfo;
  appInfo.device              = context.m_device;
  appInfo.instance            = context.m_instance;
  appInfo.physicalDevice      = context.m_physicalDevice;
  appInfo.queueIndices        = {context.m_queueGCT.familyIndex};
  appInfo.surface             = surface;
  appInfo.size.width          = SAMPLE_WIDTH;
  appInfo.size.height         = SAMPLE_HEIGHT;
  appInfo.window              = window;
  appInfo.useDynamicRendering = true;
  appInfo.useVsync = useVsync;  // V-Sync must be off for DLSS-G to work (see also sl::FeatureRequirementFlags::eVSyncOffRequired)

  StreamlineSample app;
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
  app.directFG.deviceOwner=contextOwner;
  app.directFG.retirementProbeFrame=g_frameLimit?g_frameLimit-1:0;
  if(g_directFG&&g_sdkHandoff!=SdkHandoff::none) {
    const auto& producer=sdkProducer;
    const bool cross=g_sdkHandoff==SdkHandoff::crossFamily;
    if(!producer.queue||producer.queue==context.m_queueGCT.queue||
       cross==(producer.familyIndex==context.m_queueGCT.familyIndex)){
      std::puts("SKIP: requested producer queue topology is unavailable");
      slShutdown();
      vkDestroySurfaceKHR(context.m_instance,surface,nullptr);
      if(validationMessenger) {
        auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(context.m_instance,"vkDestroyDebugUtilsMessengerEXT"));
        destroy(context.m_instance,validationMessenger,nullptr);
      }
      return 77;
    }
    app.directFG.producerQueue=producer.queue;
    app.directFG.producerFamily=producer.familyIndex;
    app.directFG.crossFamily=cross;
  }
#endif
  app.create(appInfo, dlssSupported, dlssgSupported, verifySr);
  seriesStart=std::chrono::steady_clock::now();

  // Main loop
  while(!glfwWindowShouldClose(window))
  {
    if(app.isMinimized())
    {
      glfwPollEvents();
      continue;
    }

    sl::FrameToken* frame = nullptr;
    if(SL_FAILED(res, slGetNewFrameToken(frame)))
    {
      LOGE("Streamline: Failed to get new frame token (%d)\n", res);
      break;
    }

    // Perform sleep before any input is processed for optimal frame pacing
    slReflexSleep(*frame);

    // Input
    {
      // Simulation marker should capture reading of new inputs and all the work done to update the world based on those inputs, but not the sleep above
      SAMPLE_SET_MARKER(SAMPLE_MARKER_TYPE::eSimulationStart, *frame);

      glfwPollEvents();

      app.prepareFrame();

      SAMPLE_SET_MARKER(SAMPLE_MARKER_TYPE::eSimulationEnd, *frame);
    }

    // Rendering
    {
      const uint32_t        curFrame = app.getCurFrame();
      VkCommandBuffer cmd = app.getCommandBuffers()[curFrame];

      SAMPLE_SET_MARKER(SAMPLE_MARKER_TYPE::eRenderSubmitStart, *frame);

      VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      beginInfo.flags                    = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      vkBeginCommandBuffer(cmd, &beginInfo);

      app.renderFrame(cmd, frame);

      vkEndCommandBuffer(cmd);

      SAMPLE_SET_MARKER(SAMPLE_MARKER_TYPE::eRenderSubmitEnd, *frame);
    }

    // Presentation
    {
      // This marker is required for DLSS-G to work
      SAMPLE_SET_MARKER(SAMPLE_MARKER_TYPE::ePresentStart, *frame);

      app.submitFrame();
      ++verifiedFrames;
      if((verifySr && verifiedFrames >= (g_verifyFG?12u:120u))||(g_frameLimit&&verifiedFrames>=g_frameLimit))glfwSetWindowShouldClose(window,GLFW_TRUE);

      SAMPLE_SET_MARKER(SAMPLE_MARKER_TYPE::ePresentEnd, *frame);
    }
  }

  const VkResult drained = vkDeviceWaitIdle(context.m_device);
  bool verifyPassed = !verifySr || (app.srSuccesses >= (g_verifyFG ? 12u : 120u) && app.srErrors == 0 && drained == VK_SUCCESS && (!g_verifyFG || (app.fgPresented > 0 && app.fgErrors == 0)));
  if(verifySr) std::printf("VERIFY_SR successes=%u errors=%u gpu_idle=%d output=1280x720 passed=%d\n", app.srSuccesses, app.srErrors, int(drained), int(verifyPassed));
  if(g_verifyFG) std::printf("VERIFY_FG real_frames=%u generated_presented=%u errors=%u passed=%d\n", verifiedFrames, app.fgPresented, app.fgErrors, int(verifyPassed));
  if(g_directFG){
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-seriesStart).count();
    bool passed=app.directFG.errors==0&&drained==VK_SUCCESS&&app.directFG.evaluations>0&&app.directGeneratedPresents>0&&(!g_verifyDirectFG||app.directFG.verified==app.directGeneratedPresents);
    const unsigned expectedRejections=g_missingDepthFrame>=0&&unsigned(g_missingDepthFrame)<verifiedFrames?1u:0u;
    passed=passed&&app.directFG.rejectedFrames==expectedRejections&&!app.directFG.hasInFlightInputs();
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
    if(g_cancelEvaluateFrame>=0)passed=passed&&g_cancelledEvaluate;
#endif
    printf("VERIFY_FRAMEINPUTS rejected=%u expected_rejections=%u in_flight=%d\n",app.directFG.rejectedFrames,expectedRejections,int(app.directFG.hasInFlightInputs()));
    printf("VERIFY_DIRECT_FG real_frames=%u evaluations=%u verified=%u real_presents=%u generated_presents=%u errors=%u seconds=%.6f passed=%d\n",verifiedFrames,app.directFG.evaluations,app.directFG.verified,app.directRealPresents,app.directGeneratedPresents,app.directFG.errors,seconds,int(passed));verifyPassed=passed;
  }
  app.destroy();

  // Shut down Streamline before destroying Vulkan device, so that it can clean up its resources properly
  slShutdown();

  vkDestroySurfaceKHR(context.m_instance, surface, nullptr);

  if(validationMessenger){
    auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(context.m_instance,"vkDestroyDebugUtilsMessengerEXT"));destroy(context.m_instance,validationMessenger,nullptr);
    printf("SAMPLE_VALIDATION_SUMMARY errors=%u warnings=%u\n",g_validationErrors.load(),g_validationWarnings.load());if(g_validationErrors)verifyPassed=false;
  }
#ifdef FRAMELAB_USE_INSTALLED_VULKAN_FG
  contextOwner.reset();
#else
  context.deinit();
#endif

  glfwDestroyWindow(window);
  glfwTerminate();

  return verifyPassed ? 0 : 1;
}
