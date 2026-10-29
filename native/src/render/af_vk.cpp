/*
 * ============================================================================================
 *  AvatarForge :: af_vk.cpp  --  the primary renderer: Vulkan 1.0 core, no extensions required
 * ============================================================================================
 *
 *  Written against the raw Vulkan API. Nothing here comes from a rendering framework: the
 *  instance/device setup, the swapchain, every render pass, every pipeline, the descriptor
 *  management, the shadow map, the bloom chain and the composite pass are all in this file and
 *  af_gl.cpp. The shaders are the GLSL in native/src/render/shaders/, pre-compiled to SPIR-V by
 *  tools/embed_shaders.py.
 *
 *  What the backend guarantees regardless of device:
 *      * Vulkan 1.0 core only - no dynamic rendering, no maintenance1, no KHR swapchain beyond
 *        the one extension every Android driver has. Negative viewport height is NOT used (the
 *        Y flip lives in the vertex shader) so this runs on 1.0 drivers too.
 *      * MSAA is optional: the sample count is clamped to what the device reports for both
 *        colour and depth, so a device that only supports 1x still renders correctly.
 *      * HDR: RGBA16F if it can be used as a colour attachment and sampled, else R11G11B10F,
 *        else RGBA8 (with the bloom threshold compensating - see pickHdrFormat()).
 *
 *  Frame structure (all inside one command buffer, one submit):
 *      1. shadow pass       depth-only orthographic map of the sun, 1024/2048/4096
 *      2. forward pass      MSAA colour + depth -> resolve into the single-sample HDR image;
 *                           draws the avatar submeshes, then the skeleton overlay line list
 *      3. ground pass       same pass as (2), studio plane with the same shadow term
 *      4. bloom downsample  thresholded 13-tap chain, bloomLevels levels
 *      5. bloom upsample    9-tap tent, additive, back up the chain
 *      6. composite         exposure, bloom, ACES, vignette, dither, sRGB -> swapchain image
 *
 *  Skinning is done by the kernel on the CPU (see af_render.h), so the vertex buffer is a
 *  straightforward dynamic upload of af_engine_vertices() whenever invalidateMesh() is called.
 */
#include "af_render.h"
#include "af_render_common.h"
#include "af_environment.h"
#include "af_shaders.h"

#include <android/native_window.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace af {
namespace {

constexpr int kFramesInFlight = 2;
constexpr int kMaxBloomLevels = 5;
constexpr uint32_t kPostSlices = 16;   /* one PostUbo slice per post pass                */
constexpr uint32_t kForwardTilesX = 16;
constexpr uint32_t kForwardTilesY = 9;
constexpr uint32_t kForwardMaxLights = 16;

struct ForwardLightGpu {
    float positionRange[4];
    float colorIntensity[4];
    float reserved[4];
};

struct ForwardLightHeader {
    float count;
    float width;
    float height;
    float enabled;
};

/* ------------------------------------------------------------------ helpers ---------------- */
void setError(std::string* error, const char* what, VkResult result) {
    if (!error) return;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s failed (VkResult %d)", what, int(result));
    *error = buf;
}

uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) return i;
    return 0xFFFFFFFFu;
}

const char* vkDeviceTypeName(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU";
        default:                                     return "other";
    }
}

/* ============================================================================================
 *  VulkanRenderer
 * ============================================================================================ */
class VulkanRenderer final : public Renderer {
public:
    bool init(ANativeWindow* window, const RendererOptions& options, std::string* error);
    ~VulkanRenderer() override;

    bool resize(int width, int height) override;
    void attachEngine(af_engine* engine) override;
    void invalidateMesh() override;
    void setCamera(const CameraState& camera) override;
    void setOptions(const RendererOptions& options) override;
    RendererOptions options() const override { return options_; }
    void renderFrame(float deltaSeconds) override;
    void setSkeletonVisible(bool visible) override { skeletonVisible_ = visible; }
    void setFlatShading(bool flat) override { flatShading_ = flat; }
    void setHighlightRegion(int region) override { highlightRegion_ = region; }
    void setClip(int clip, float speed, bool loop) override;
    int currentClip() const override;
    float clipTime() const override;
    void waitIdle() override;
    RendererStats stats() const override { return stats_; }
    std::string frameError() const override { return frameError_; }
    RendererInfo info() const override { return info_; }

private:
    /* ---- setup steps (each returns false and fills error on failure) ---- */
    bool createInstance(std::string* error);
    bool createSurface(ANativeWindow* window, std::string* error);
    bool pickPhysicalDevice(std::string* error);
    bool createDevice(std::string* error);
    bool createSwapchain(std::string* error, int width, int height);
    bool createShadowTarget(std::string* error);
    bool createHdrTargets(std::string* error);
    bool createSamplers(std::string* error);
    bool createDescriptorLayouts(std::string* error);
    bool createDescriptorPool(std::string* error);
    bool writeDescriptorSets(std::string* error);
    bool createPipelines(std::string* error);
    bool createBuffers(std::string* error);
    bool uploadAtlas(std::string* error);
    void destroySwapchainResources();
    void destroyPipelines();
    void destroyAll();

    /* ---- per-frame ---- */
    void updateScene(float deltaSeconds);
    void updateAnimation(float deltaSeconds);
    void uploadDynamicGeometry(VkCommandBuffer cmd);
    void writePostSlices();
    void recordCommands(VkCommandBuffer cmd, uint32_t imageIndex);
    void recordShadowPass(VkCommandBuffer cmd);
    void recordForwardPass(VkCommandBuffer cmd, uint32_t imageIndex);

    /* ---- resource helpers ---- */
    VkShaderModule makeModule(const AfShaderVk* blob);
    bool createImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                     VkImage* image, VkDeviceMemory* memory, uint32_t levels = 1);
    bool createView(VkImage image, VkFormat format, VkImageAspectFlags aspect, VkImageView* view,
                    uint32_t levels = 1);
    VkFormat pickHdrFormat() const;
    bool createFallbackAtlas(std::string* error);
    void rebuildMaterialBlocks();
    void buildSkeletonLines();
    void applyOptionChanges();
    void destroyTargets();
    bool createBuffer(uint64_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                      VkBuffer* buffer, VkDeviceMemory* memory, void** mapped);
    bool createRenderTarget(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                            VkImage* image, VkDeviceMemory* memory, VkImageView* view,
                            VkImageAspectFlags aspect,
                            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    bool createFramebuffer(VkRenderPass pass, const std::vector<VkImageView>& views, uint32_t w,
                           uint32_t h, VkFramebuffer* out);
    void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                      VkImageAspectFlags aspect);
    void generateMips(VkCommandBuffer cmd, VkImage image, int32_t width, int32_t height,
                      uint32_t levels);

    /* ================= state ================= */
    RendererOptions options_{};
    RendererOptions activeTargetOptions_{};  /* last target/pipeline set that built successfully */
    RendererInfo info_{};
    RendererStats stats_{};
    std::string frameError_;
    bool frameFatal_ = false;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkPhysicalDeviceMemoryProperties memProps_{};
    uint32_t uboAlignment_ = 256;
    uint32_t cameraStride_ = 512;
    bool deviceSupportsCompareSamplerLinear_ = false;

    /* swapchain */
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR swapColorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D swapExtent_{0, 0};
    std::vector<VkImage> swapImages_;
    std::vector<VkImageView> swapViews_;
    VkRenderPass compositePass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> swapFramebuffers_;
    std::vector<VkSemaphore> renderFinished_;      /* one per swapchain image */
    std::vector<VkSemaphore> imageAvailable_;      /* kFramesInFlight         */
    std::vector<VkFence> inFlight_;
    std::vector<VkCommandBuffer> cmd_;
    std::vector<VkBuffer> cameraUbos_;
    std::vector<VkDeviceMemory> cameraUboMem_;
    std::vector<void*> cameraUboMapped_;
    uint32_t frameIndex_ = 0;
    bool framebufferResized_ = false;

    /* targets */
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;
    VkFormat hdrFormat_ = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkImage depthImage_ = VK_NULL_HANDLE;      VkDeviceMemory depthMem_ = VK_NULL_HANDLE;
    VkImageView depthView_ = VK_NULL_HANDLE;
    VkImage msaaImage_ = VK_NULL_HANDLE;       VkDeviceMemory msaaMem_ = VK_NULL_HANDLE;
    VkImageView msaaView_ = VK_NULL_HANDLE;
    VkImage hdrImage_ = VK_NULL_HANDLE;        VkDeviceMemory hdrMem_ = VK_NULL_HANDLE;
    VkImageView hdrView_ = VK_NULL_HANDLE;
    VkImage shadowImage_ = VK_NULL_HANDLE;     VkDeviceMemory shadowMem_ = VK_NULL_HANDLE;
    VkImageView shadowView_ = VK_NULL_HANDLE;

    /* bloom */
    struct BloomLevel {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
    };
    BloomLevel bloom_[kMaxBloomLevels];
    int bloomLevels_ = 0;

    /* render passes and framebuffers */
    VkRenderPass shadowPass_ = VK_NULL_HANDLE;
    VkFramebuffer shadowFramebuffer_ = VK_NULL_HANDLE;
    VkRenderPass hdrPass_ = VK_NULL_HANDLE;     /* MSAA colour + resolve + depth, or 1x HDR + depth */
    VkFramebuffer hdrFramebuffer_ = VK_NULL_HANDLE;
    VkRenderPass bloomDownPass_ = VK_NULL_HANDLE;
    VkRenderPass bloomUpPass_ = VK_NULL_HANDLE;

    /* pipelines */
    VkPipelineLayout avatarLayout_ = VK_NULL_HANDLE;   /* set0 camera + set1 material */
    VkPipelineLayout forwardPlusLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout postLayout_ = VK_NULL_HANDLE;     /* set0 only                   */
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipeline forwardPipeline_ = VK_NULL_HANDLE;
    VkPipeline groundPipeline_ = VK_NULL_HANDLE;
    VkPipeline linePipeline_ = VK_NULL_HANDLE;
    VkPipeline bloomDownPipeline_ = VK_NULL_HANDLE;
    VkPipeline bloomUpPipeline_ = VK_NULL_HANDLE;
    VkPipeline compositePipeline_ = VK_NULL_HANDLE;
    VkPipeline forwardPlusPipeline_ = VK_NULL_HANDLE;

    /* descriptors */
    VkDescriptorSetLayout setLayout0_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout1_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout2_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> cameraSets_;     /* kFramesInFlight                    */
    VkDescriptorSet materialSet_ = VK_NULL_HANDLE;      /* set 1, dynamic offset       */
    std::vector<VkDescriptorSet> bloomDownSets_;  /* per level                          */
    std::vector<VkDescriptorSet> bloomUpSets_;    /* per level                          */
    VkDescriptorSet compositeSet_ = VK_NULL_HANDLE;
    VkDescriptorSet lineSet_ = VK_NULL_HANDLE;
    VkDescriptorSet forwardPlusSet_ = VK_NULL_HANDLE;

    VkSampler atlasSampler_ = VK_NULL_HANDLE;
    VkSampler shadowSampler_ = VK_NULL_HANDLE;
    VkSampler postSampler_ = VK_NULL_HANDLE;

    /* post-process uniform buffer, sliced per pass */
    VkBuffer postUbo_ = VK_NULL_HANDLE;
    VkDeviceMemory postUboMem_ = VK_NULL_HANDLE;
    void* postUboMapped_ = nullptr;
    VkBuffer postLineUbo_ = VK_NULL_HANDLE;    /* unused slot, keeps the table dense */

    /* geometry */
    VkImageView atlasViews_[AF_TEX_COUNT] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                             VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImage atlasImages_[AF_TEX_COUNT] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                          VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory atlasMemory_[AF_TEX_COUNT] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                                 VK_NULL_HANDLE, VK_NULL_HANDLE};
    int atlasWidth_ = 0, atlasHeight_ = 0;
    bool atlasUploaded_ = false;

    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertexMem_ = VK_NULL_HANDLE;
    void* vertexMapped_ = nullptr;
    uint64_t vertexCapacity_ = 0;
    int32_t vertexCount_ = 0;
    VkBuffer indexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory indexMem_ = VK_NULL_HANDLE;
    void* indexMapped_ = nullptr;
    uint64_t indexCapacity_ = 0;
    int32_t indexCount_ = 0;

    /* CPU-populated, GPU-consumed draw arguments.  The command buffer still binds each
       submesh's material independently, but index/instance counts no longer travel through
       vkCmdDrawIndexed's scalar arguments.  This is the safe Vulkan-1.0 baseline for the
       GPU-driven path; a future compute pass can overwrite the same buffer after culling. */
    VkBuffer indirectBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory indirectMem_ = VK_NULL_HANDLE;
    void* indirectMapped_ = nullptr;
    uint64_t indirectCapacity_ = 0;
    uint32_t indirectCommandCount_ = 0;

    VkBuffer forwardLightsBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory forwardLightsMem_ = VK_NULL_HANDLE;
    void* forwardLightsMapped_ = nullptr;
    VkBuffer forwardTilesBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory forwardTilesMem_ = VK_NULL_HANDLE;
    void* forwardTilesMapped_ = nullptr;

    VkBuffer materialUbo_ = VK_NULL_HANDLE;
    VkDeviceMemory materialUboMem_ = VK_NULL_HANDLE;
    void* materialUboMapped_ = nullptr;
    uint64_t materialUboSize_ = 0;
    uint32_t materialStride_ = 256;
    int materialCount_ = 0;

    VkBuffer groundBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory groundMem_ = VK_NULL_HANDLE;
    VkBuffer lineBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory lineMem_ = VK_NULL_HANDLE;
    void* lineMapped_ = nullptr;
    uint64_t lineCapacity_ = 0;
    uint32_t lineVertexCount_ = 0;

    /* scene */
    af_engine* engine_ = nullptr;
    CameraState camera_{};
    FrameScene scene_{};
    EnvironmentParams env_{};
    ShGlobal sh_{};
    bool meshDirty_ = true;
    bool atlasDirty_ = true;
    int highlightRegion_ = -1;
    bool skeletonVisible_ = false;
    bool flatShading_ = false;
    bool targetsDirty_ = false;
    bool highlightDirty_ = true;
    int clip_ = -1;
    float clipSpeed_ = 1.0f;
    bool clipLoop_ = true;
    float clipTime_ = 0.0f;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    int drawCallsThisFrame_ = 0;
    int trianglesThisFrame_ = 0;
};

/* ============================================================================================
 *  Initialisation
 * ============================================================================================ */
bool VulkanRenderer::init(ANativeWindow* window, const RendererOptions& options, std::string* error) {
    options_ = options;
    if (!createInstance(error)) return false;
    if (!createSurface(window, error)) return false;
    if (!pickPhysicalDevice(error)) return false;
    if (!createDevice(error)) return false;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys_, &props);
    uboAlignment_ = props.limits.minUniformBufferOffsetAlignment ? props.limits.minUniformBufferOffsetAlignment : 256;
    cameraStride_ = uint32_t((sizeof(CameraUbo) + uboAlignment_ - 1) / uboAlignment_) * uboAlignment_;

    /* Sample count: clamp to what the device allows for both colour and depth. */
    VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
    uint32_t want = uint32_t(options_.msaaSamples);
    if (want < 1) want = 1;
    if (want > 8) want = 8;
    uint32_t chosen = 1;
    for (int s : {8, 4, 2}) {
        if (want >= uint32_t(s) && (counts & VkSampleCountFlagBits(s))) { chosen = uint32_t(s); break; }
    }
    options_.msaaSamples = int(chosen);
    info_.supportsMsaa = counts != 0;

    if (!createSwapchain(error, 0, 0)) return false;
    if (!createShadowTarget(error)) return false;
    if (!createHdrTargets(error)) return false;
    if (!createSamplers(error)) return false;
    if (!createDescriptorLayouts(error)) return false;
    if (!createBuffers(error)) return false;
    if (!createFallbackAtlas(error)) return false;
    if (!createPipelines(error)) return false;
    /* The engine may already carry a baked atlas (the viewer opens after generation), in which
       case it goes straight to the GPU; otherwise the 1x1 placeholders stay bound. */
    if (engine_) uploadAtlas(error);

    info_.backend = "vulkan";
    info_.deviceName = props.deviceName;
    char apiBuf[96];
    std::snprintf(apiBuf, sizeof(apiBuf), "Vulkan %u.%u.%u",
                  VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                  VK_VERSION_PATCH(props.apiVersion));
    info_.apiVersion = apiBuf;
    char drvBuf[96];
    std::snprintf(drvBuf, sizeof(drvBuf), "driver %u (%s)", props.driverVersion, vkDeviceTypeName(props.deviceType));
    info_.driver = drvBuf;
    info_.supportsShadows = shadowImage_ != VK_NULL_HANDLE;
    info_.supportsFloatHdr = hdrFormat_ == VK_FORMAT_R16G16B16A16_SFLOAT;
    stats_.msaaSamples = options_.msaaSamples;
    stats_.shadowMapSize = options_.enableShadows ? options_.shadowMapSize : 0;
    stats_.bloomLevels = options_.enableBloom ? bloomLevels_ : 0;
    activeTargetOptions_ = options_;
    return true;
}

bool VulkanRenderer::createInstance(std::string* error) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "AvatarForge";
    app.applicationVersion = 1;
    app.pEngineName = "AvatarForge";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_0;   /* 1.0 core only: every Android Vulkan driver qualifies */

    const char* extensions[] = {"VK_KHR_surface", "VK_KHR_android_surface"};
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = extensions;
    /* No validation layers are requested: the app ships without the layer binaries, and the
       layer list is a debug-time concern. Release builds stay silent. */
    const VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        setError(error, "vkCreateInstance", r);
        return false;
    }
    return true;
}

bool VulkanRenderer::createSurface(ANativeWindow* window, std::string* error) {
    VkAndroidSurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    ci.window = window;
    const VkResult r = vkCreateAndroidSurfaceKHR(instance_, &ci, nullptr, &surface_);
    if (r != VK_SUCCESS) {
        setError(error, "vkCreateAndroidSurfaceKHR", r);
        return false;
    }
    return true;
}

bool VulkanRenderer::pickPhysicalDevice(std::string* error) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        *error = "no Vulkan device present";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    VkPhysicalDevice best = VK_NULL_HANDLE;
    int bestScore = -1;
    uint32_t bestFamily = 0;
    for (VkPhysicalDevice dev : devices) {
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, queues.data());
        for (uint32_t q = 0; q < qcount; ++q) {
            if (!(queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, q, surface_, &present);
            if (!present) continue;
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(dev, &p);
            int score = 0;
            switch (p.deviceType) {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   score = 400; break;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 300; break;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    score = 200; break;
                default:                                     score = 100; break;
            }
            /* A device that can do 4x MSAA on both colour and depth is preferred: it is what the
               viewer wants for hair and the high-contrast eye highlight. */
            const VkSampleCountFlags sc = p.limits.framebufferColorSampleCounts & p.limits.framebufferDepthSampleCounts;
            if (sc & VK_SAMPLE_COUNT_4_BIT) score += 50;
            if (score > bestScore) { bestScore = score; best = dev; bestFamily = q; }
            break;
        }
    }
    if (best == VK_NULL_HANDLE) {
        *error = "no Vulkan device with a graphics+present queue for this surface";
        return false;
    }
    phys_ = best;
    queueFamily_ = bestFamily;
    vkGetPhysicalDeviceMemoryProperties(phys_, &memProps_);
    return true;
}

bool VulkanRenderer::createDevice(std::string* error) {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = queueFamily_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;

    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(phys_, &supported);
    VkPhysicalDeviceFeatures features{};
    features.samplerAnisotropy = supported.samplerAnisotropy;
    features.depthClamp = supported.depthClamp;
    /* Wide lines are optional and unreliable on mobile; the skeleton overlay uses 1 px lines and
       the fragment shader keeps them readable, so the feature is never requested. */

    const char* extensions[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = extensions;
    ci.pEnabledFeatures = &features;
    const VkResult r = vkCreateDevice(phys_, &ci, nullptr, &device_);
    if (r != VK_SUCCESS) {
        setError(error, "vkCreateDevice", r);
        return false;
    }
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    info_.supportsAnisotropy = supported.samplerAnisotropy == VK_TRUE;
    return true;
}

VkShaderModule VulkanRenderer::makeModule(const AfShaderVk* blob) {
    if (!blob) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = size_t(blob->wordCount) * 4;
    ci.pCode = blob->words;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &ci, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

/* ============================================================================================
 *  Swapchain and render targets
 * ============================================================================================ */
#define AF_VK_CHECK(x) do { VkResult af_r_ = (x); if (af_r_ != VK_SUCCESS) { setError(error, #x, af_r_); return false; } } while (0)

bool VulkanRenderer::createSwapchain(std::string* error, int width, int height) {
    VkSurfaceCapabilitiesKHR caps{};
    AF_VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps));

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount ? formatCount : 1);
    if (formatCount) vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &formatCount, formats.data());
    else formats[0] = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};

    VkSurfaceFormatKHR chosen = formats[0];
    for (const VkSurfaceFormatKHR& f : formats) {
        const bool srgb = (f.format == VK_FORMAT_R8G8B8A8_SRGB || f.format == VK_FORMAT_B8G8R8A8_SRGB);
        if (srgb && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
    }
    swapFormat_ = chosen.format;
    swapColorSpace_ = chosen.colorSpace;

    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount ? modeCount : 1);
    if (modeCount) vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &modeCount, modes.data());
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;   /* always available, vsync'd */
    for (VkPresentModeKHR m : modes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) { presentMode = m; break; }   /* lowest latency */
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX || extent.height == UINT32_MAX) {
        VkExtent2D want{uint32_t(width > 0 ? width : 1080), uint32_t(height > 0 ? height : 1920)};
        if (caps.currentExtent.width != UINT32_MAX) want = caps.currentExtent;
        want.width = std::max(caps.minImageExtent.width, std::min(caps.maxImageExtent.width, want.width));
        want.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, want.height));
        extent = want;
    }
    swapExtent_ = extent;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        const VkCompositeAlphaFlagBitsKHR fallbacks[3] = {
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR};
        for (VkCompositeAlphaFlagBitsKHR f : fallbacks)
            if (caps.supportedCompositeAlpha & f) { alpha = f; break; }
    }

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface_;
    ci.minImageCount = imageCount;
    ci.imageFormat = swapFormat_;
    ci.imageColorSpace = swapColorSpace_;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain_;
    AF_VK_CHECK(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_));
    if (ci.oldSwapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, ci.oldSwapchain, nullptr);

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    swapImages_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, swapImages_.data());

    swapViews_.resize(n, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n; ++i)
        if (!createView(swapImages_[i], swapFormat_, VK_IMAGE_ASPECT_COLOR_BIT, &swapViews_[i])) {
            *error = "vkCreateImageView(swapchain) failed";
            return false;
        }

    /* ---- composite render pass ------------------------------------------------------------------
       The final pass writes straight into the swapchain image: single sample, no depth, cleared
       to nothing because every pixel is covered by the fullscreen triangle. */
    VkAttachmentDescription color{};
    color.format = swapFormat_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    AF_VK_CHECK(vkCreateRenderPass(device_, &rp, nullptr, &compositePass_));

    swapFramebuffers_.resize(n, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n; ++i)
        if (!createFramebuffer(compositePass_, {swapViews_[i]}, extent.width, extent.height,
                               &swapFramebuffers_[i])) {
            *error = "vkCreateFramebuffer(composite) failed";
            return false;
        }

    /* ---- per-frame synchronisation and command buffers ------------------------------------------ */
    if (imageAvailable_.empty()) {
        imageAvailable_.resize(kFramesInFlight);
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (int i = 0; i < kFramesInFlight; ++i)
            AF_VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &imageAvailable_[i]));

        renderFinished_.resize(n, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < n; ++i)
            AF_VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &renderFinished_[i]));

        inFlight_.resize(kFramesInFlight);
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (int i = 0; i < kFramesInFlight; ++i)
            AF_VK_CHECK(vkCreateFence(device_, &fi, nullptr, &inFlight_[i]));

        VkCommandPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = queueFamily_;
        AF_VK_CHECK(vkCreateCommandPool(device_, &pool, nullptr, &commandPool_));

        cmd_.resize(kFramesInFlight, VK_NULL_HANDLE);
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = commandPool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = kFramesInFlight;
        AF_VK_CHECK(vkAllocateCommandBuffers(device_, &ai, cmd_.data()));

        /* Camera uniform buffer: one slice per frame in flight, bound with a dynamic offset. The
           two slices are what keep frame N+1 from overwriting the camera frame N is still using. */
        const uint64_t camSize = uint64_t(cameraStride_) * kFramesInFlight;
        if (!createBuffer(camSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &cameraUbos_.emplace_back(), &cameraUboMem_.emplace_back(),
                          &cameraUboMapped_.emplace_back())) {
            *error = "camera uniform buffer allocation failed";
            return false;
        }
    } else {
        /* Recreating the swapchain (rotation/resize): the per-image semaphores must match the new
           image count, everything else is reusable. */
        for (VkSemaphore s : renderFinished_) vkDestroySemaphore(device_, s, nullptr);
        renderFinished_.assign(n, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (uint32_t i = 0; i < n; ++i)
            AF_VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &renderFinished_[i]));
    }
    return true;
}

bool VulkanRenderer::createShadowTarget(std::string* error) {
    /* One orthographic map wrapped around the figure. 1024 is the floor even when shadows are
       switched off, because the texture has to exist for the descriptor set to be valid. */
    int size = options_.enableShadows ? options_.shadowMapSize : 512;
    if (size < 512) size = 512;
    if (size > 4096) size = 4096;
    options_.shadowMapSize = size;

    if (!createRenderTarget(uint32_t(size), uint32_t(size), depthFormat_,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            &shadowImage_, &shadowMem_, &shadowView_, VK_IMAGE_ASPECT_DEPTH_BIT)) {
        *error = "shadow map allocation failed";
        return false;
    }

    VkAttachmentDescription depth{};
    depth.format = depthFormat_;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 0;
    sub.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;   /* previous frame's lookup */
    dep.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments = &depth;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    AF_VK_CHECK(vkCreateRenderPass(device_, &rp, nullptr, &shadowPass_));
    if (!createFramebuffer(shadowPass_, {shadowView_}, uint32_t(size), uint32_t(size),
                           &shadowFramebuffer_)) {
        *error = "shadow framebuffer failed";
        return false;
    }
    return true;
}

VkFormat VulkanRenderer::pickHdrFormat() const {
    /* Ordered by preference. RGBA16F is the real target (it is what keeps the sun disc and the
       eye highlight above 1.0 for the bloom to find); R11G11B10F is the mobile-friendly
       compromise; RGBA8 keeps the app running on a strict device with the threshold lowered by
       the caller so bloom still triggers, and nothing else changes. */
    const VkFormat candidates[3] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                                    VK_FORMAT_R8G8B8A8_UNORM};
    const VkImageUsageFlags need = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (VkFormat f : candidates) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(phys_, f, &fp);
        if ((fp.optimalTilingFeatures & need) == need) return f;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

bool VulkanRenderer::createHdrTargets(std::string* error) {
    hdrFormat_ = pickHdrFormat();
    const uint32_t w = swapExtent_.width, h = swapExtent_.height;
    VkSampleCountFlagBits samples = VkSampleCountFlagBits(options_.msaaSamples < 1 ? 1 : options_.msaaSamples);

    /* Depth (MSAA, matches the colour sample count) */
    if (!createRenderTarget(w, h, depthFormat_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                            &depthImage_, &depthMem_, &depthView_, VK_IMAGE_ASPECT_DEPTH_BIT,
                            samples)) {
        *error = "depth target allocation failed";
        return false;
    }
    /* MSAA colour (only when the device gave us > 1 sample) */
    if (samples != VK_SAMPLE_COUNT_1_BIT) {
        if (!createRenderTarget(w, h, hdrFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                &msaaImage_, &msaaMem_, &msaaView_, VK_IMAGE_ASPECT_COLOR_BIT,
                                samples)) {
            *error = "MSAA colour target allocation failed";
            options_.msaaSamples = 1;
            samples = VK_SAMPLE_COUNT_1_BIT;
        }
    }
    /* Single-sample HDR resolve target: sampled by bloom, composite and nothing else. */
    if (!createRenderTarget(w, h, hdrFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            &hdrImage_, &hdrMem_, &hdrView_, VK_IMAGE_ASPECT_COLOR_BIT)) {
        *error = "HDR target allocation failed";
        return false;
    }

    /* ---- forward render pass: 1 (or 2) colour attachments + depth ------------------------------- */
    std::vector<VkAttachmentDescription> attachments;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolveRef{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkAttachmentDescription colorAtt{};
    colorAtt.format = hdrFormat_;
    colorAtt.samples = samples;
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt.storeOp = (samples == VK_SAMPLE_COUNT_1_BIT) ? VK_ATTACHMENT_STORE_OP_STORE
                                                          : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAtt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAtt.finalLayout = (samples == VK_SAMPLE_COUNT_1_BIT) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                              : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments.push_back(colorAtt);

    if (samples != VK_SAMPLE_COUNT_1_BIT) {
        VkAttachmentDescription resolve{};
        resolve.format = hdrFormat_;
        resolve.samples = VK_SAMPLE_COUNT_1_BIT;
        resolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        resolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        resolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        resolve.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachments.push_back(resolve);
        depthRef.attachment = 2;
    } else {
        depthRef.attachment = 1;
    }

    VkAttachmentDescription depthAtt{};
    depthAtt.format = depthFormat_;
    depthAtt.samples = samples;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAtt.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments.push_back(depthAtt);

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    if (samples != VK_SAMPLE_COUNT_1_BIT) {
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        sub.pResolveAttachments = &resolveRef;
    }
    sub.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = uint32_t(attachments.size());
    rp.pAttachments = attachments.data();
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    AF_VK_CHECK(vkCreateRenderPass(device_, &rp, nullptr, &hdrPass_));

    std::vector<VkImageView> views;
    views.push_back(samples == VK_SAMPLE_COUNT_1_BIT ? hdrView_ : msaaView_);
    if (samples != VK_SAMPLE_COUNT_1_BIT) views.push_back(hdrView_);
    views.push_back(depthView_);
    if (!createFramebuffer(hdrPass_, views, w, h, &hdrFramebuffer_)) {
        *error = "HDR framebuffer failed";
        return false;
    }

    /* ---- bloom chain ---------------------------------------------------------------------------- */
    bloomLevels_ = options_.enableBloom ? std::max(1, std::min(kMaxBloomLevels, options_.bloomLevels)) : 0;
    if (bloomLevels_ > 0) {
        VkAttachmentDescription ba{};
        ba.format = hdrFormat_;
        ba.samples = VK_SAMPLE_COUNT_1_BIT;
        ba.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ba.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ba.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ba.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ba.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ba.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription bs{};
        bs.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        bs.colorAttachmentCount = 1;
        bs.pColorAttachments = &ar;
        VkSubpassDependency bd{};
        bd.srcSubpass = VK_SUBPASS_EXTERNAL;
        bd.dstSubpass = 0;
        bd.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        bd.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        bd.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bd.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo brp{};
        brp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        brp.attachmentCount = 1;
        brp.pAttachments = &ba;
        brp.subpassCount = 1;
        brp.pSubpasses = &bs;
        brp.dependencyCount = 1;
        brp.pDependencies = &bd;
        AF_VK_CHECK(vkCreateRenderPass(device_, &brp, nullptr, &bloomDownPass_));
        /* The upsample pass loads the finer level and adds into it, so it must both keep its
           contents (LOAD) and end up sampled by the next pass up (SHADER_READ_ONLY). */
        ba.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ba.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bd.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        bd.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        AF_VK_CHECK(vkCreateRenderPass(device_, &brp, nullptr, &bloomUpPass_));

        int level = 0;
        for (int i = 0; i < bloomLevels_; ++i) {
            uint32_t lw = std::max(1u, w >> (i + 1));
            uint32_t lh = std::max(1u, h >> (i + 1));
            if (lw < 8 || lh < 8) break;   /* nothing useful left to blur */
            BloomLevel& bl = bloom_[level];
            if (!createRenderTarget(lw, lh, hdrFormat_,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                    &bl.image, &bl.memory, &bl.view, VK_IMAGE_ASPECT_COLOR_BIT)) {
                *error = "bloom level allocation failed";
                return false;
            }
            bl.width = lw;
            bl.height = lh;
            if (!createFramebuffer(bloomDownPass_, {bl.view}, lw, lh, &bl.framebuffer)) {
                *error = "bloom framebuffer failed";
                return false;
            }
            ++level;
        }
        bloomLevels_ = level;
    }
    stats_.bloomLevels = bloomLevels_;
    stats_.msaaSamples = options_.msaaSamples;
    return true;
}

bool VulkanRenderer::createSamplers(std::string* error) {
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 12.0f;
    if (info_.supportsAnisotropy) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = 4.0f;   /* 4x is the sweet spot on mobile: 16x costs without visible gain */
    }
    AF_VK_CHECK(vkCreateSampler(device_, &si, nullptr, &atlasSampler_));

    /* The shadow sampler compares in hardware: 2x2 linear PCF for free, on top of the 5 taps the
       shader issues. */
    VkSamplerCreateInfo ss{};
    ss.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ss.magFilter = VK_FILTER_LINEAR;
    ss.minFilter = VK_FILTER_LINEAR;
    ss.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ss.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ss.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ss.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ss.compareEnable = VK_TRUE;
    ss.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    ss.maxLod = 0.0f;
    AF_VK_CHECK(vkCreateSampler(device_, &ss, nullptr, &shadowSampler_));

    VkSamplerCreateInfo ps{};
    ps.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ps.magFilter = VK_FILTER_LINEAR;
    ps.minFilter = VK_FILTER_LINEAR;
    ps.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ps.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ps.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ps.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ps.maxLod = 0.0f;
    AF_VK_CHECK(vkCreateSampler(device_, &ps, nullptr, &postSampler_));
    return true;
}

bool VulkanRenderer::createDescriptorLayouts(std::string* error) {
    /* Set 0: camera UBO (dynamic offset per frame), shadow sampler, post UBO (dynamic offset per
       pass), plus the two samplers the post passes read from. */
    VkDescriptorSetLayoutBinding b0[5]{};
    b0[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b0[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b0[2] = {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b0[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b0[4] = {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 5;
    li.pBindings = b0;
    AF_VK_CHECK(vkCreateDescriptorSetLayout(device_, &li, nullptr, &setLayout0_));

    /* Set 1: per-submesh material block (dynamic offset) + the five atlas maps. */
    VkDescriptorSetLayoutBinding b1[6]{};
    b1[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (int i = 0; i < 5; ++i)
        b1[i + 1] = {uint32_t(i + 1), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                     VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    li.bindingCount = 6;
    li.pBindings = b1;
    AF_VK_CHECK(vkCreateDescriptorSetLayout(device_, &li, nullptr, &setLayout1_));

    /* Set 2: Forward+ light input and GPU-written tile lists. */
    VkDescriptorSetLayoutBinding b2[2]{};
    b2[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
             VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b2[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
             VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    li.bindingCount = 2;
    li.pBindings = b2;
    AF_VK_CHECK(vkCreateDescriptorSetLayout(device_, &li, nullptr, &setLayout2_));
    return true;
}

bool VulkanRenderer::createBuffers(std::string* error) {
    /* Post-process uniform slices: one per bloom down, one per bloom up, one for the composite. */
    const uint64_t postSize = uint64_t(uboAlignment_) * kPostSlices;
    if (!createBuffer(postSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &postUbo_, &postUboMem_, &postUboMapped_)) {
        *error = "post uniform buffer allocation failed";
        return false;
    }

    const uint64_t lightBytes = sizeof(ForwardLightHeader) + sizeof(ForwardLightGpu) * kForwardMaxLights;
    if (!createBuffer(lightBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &forwardLightsBuffer_, &forwardLightsMem_, &forwardLightsMapped_)) {
        *error = "Forward+ light buffer allocation failed";
        return false;
    }
    if (!createBuffer(uint64_t(kForwardTilesX) * kForwardTilesY * sizeof(uint32_t) * 4,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &forwardTilesBuffer_, &forwardTilesMem_, &forwardTilesMapped_)) {
        *error = "Forward+ tile buffer allocation failed";
        return false;
    }
    auto* header = static_cast<ForwardLightHeader*>(forwardLightsMapped_);
    header->count = 8.0f;
    header->width = float(std::max(swapExtent_.width, 1u));
    header->height = float(std::max(swapExtent_.height, 1u));
    header->enabled = 1.0f;
    auto* lights = reinterpret_cast<ForwardLightGpu*>(header + 1);
    for (uint32_t i = 0; i < 8; ++i) {
        const float angle = float(i) * 0.78539816339f;
        lights[i].positionRange[0] = std::cos(angle) * 1.8f;
        lights[i].positionRange[1] = 1.0f + 0.35f * std::sin(angle * 2.0f);
        lights[i].positionRange[2] = std::sin(angle) * 1.8f;
        lights[i].positionRange[3] = 2.5f;
        lights[i].colorIntensity[0] = 0.65f + 0.35f * std::cos(angle);
        lights[i].colorIntensity[1] = 0.65f + 0.35f * std::cos(angle + 2.094f);
        lights[i].colorIntensity[2] = 0.65f + 0.35f * std::cos(angle + 4.188f);
        lights[i].colorIntensity[3] = 1.0f;
    }
    std::memset(static_cast<uint8_t*>(forwardTilesMapped_), 0,
                size_t(kForwardTilesX) * kForwardTilesY * sizeof(uint32_t) * 4);

    /* Studio ground quad: two triangles in the XZ plane, positions only (the shader ignores y). */
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    void* stagingMap = nullptr;
    if (!createBuffer(sizeof(float) * 3 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &staging, &stagingMem, &stagingMap)) {
        *error = "ground staging buffer failed";
        return false;
    }
    const float quad[12] = {-1.0f, 0.0f, -1.0f, 1.0f, 0.0f, -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    std::memcpy(stagingMap, quad, sizeof(quad));
    if (!createBuffer(sizeof(quad), VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &groundBuffer_, &groundMem_, nullptr)) {
        *error = "ground vertex buffer failed";
        return false;
    }
    /* One-shot copy. */
    {
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = commandPool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.queueFamilyIndex = queueFamily_;
        VkCommandPool tmpPool = VK_NULL_HANDLE;
        if (commandPool_ == VK_NULL_HANDLE) {
            vkCreateCommandPool(device_, &pool, nullptr, &tmpPool);
            ai.commandPool = tmpPool;
        }
        vkAllocateCommandBuffers(device_, &ai, &cb);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        VkBufferCopy copy{0, 0, sizeof(quad)};
        vkCmdCopyBuffer(cb, staging, groundBuffer_, 1, &copy);
        VkBufferMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.buffer = groundBuffer_;
        bar.offset = 0;
        bar.size = sizeof(quad);
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                             0, 0, nullptr, 1, &bar, 0, nullptr);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue_);
        vkFreeCommandBuffers(device_, ai.commandPool, 1, &cb);
        if (tmpPool != VK_NULL_HANDLE) vkDestroyCommandPool(device_, tmpPool, nullptr);
    }
    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);

    /* Skeleton overlay: one segment per joint (71 joints, 2 vertices each, 28 bytes per vertex)
       with generous headroom for future rig revisions. */
    lineCapacity_ = 4096 * 28;
    if (!createBuffer(lineCapacity_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &lineBuffer_, &lineMem_, &lineMapped_)) {
        *error = "skeleton line buffer allocation failed";
        return false;
    }
    return true;
}

/* ---- low level resource helpers ------------------------------------------------------------- */

bool VulkanRenderer::createBuffer(uint64_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                                  VkBuffer* buffer, VkDeviceMemory* memory, void** mapped) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size ? size : 16;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bi, nullptr, buffer) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, *buffer, &req);
    const uint32_t type = findMemoryType(phys_, req.memoryTypeBits, props);
    if (type == 0xFFFFFFFFu) return false;

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &ai, nullptr, memory) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(device_, *buffer, *memory, 0) != VK_SUCCESS) return false;
    if (mapped) {
        if (vkMapMemory(device_, *memory, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS) return false;
    }
    return true;
}

bool VulkanRenderer::createImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                                 VkImage* image, VkDeviceMemory* memory, uint32_t levels) {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = levels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ci, nullptr, image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, *image, &req);
    const uint32_t type = findMemoryType(phys_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == 0xFFFFFFFFu) return false;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &ai, nullptr, memory) != VK_SUCCESS) return false;
    return vkBindImageMemory(device_, *image, *memory, 0) == VK_SUCCESS;
}

bool VulkanRenderer::createView(VkImage image, VkFormat format, VkImageAspectFlags aspect,
                               VkImageView* view, uint32_t levels) {
    VkImageViewCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = image;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = format;
    ci.subresourceRange.aspectMask = aspect;
    ci.subresourceRange.levelCount = levels;
    ci.subresourceRange.layerCount = 1;
    return vkCreateImageView(device_, &ci, nullptr, view) == VK_SUCCESS;
}

bool VulkanRenderer::createRenderTarget(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                                        VkImage* image, VkDeviceMemory* memory, VkImageView* view,
                                        VkImageAspectFlags aspect, VkSampleCountFlagBits samples) {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ci, nullptr, image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, *image, &req);
    const uint32_t type = findMemoryType(phys_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == 0xFFFFFFFFu) return false;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &ai, nullptr, memory) != VK_SUCCESS) return false;
    if (vkBindImageMemory(device_, *image, *memory, 0) != VK_SUCCESS) return false;
    return createView(*image, format, aspect, view);
}

bool VulkanRenderer::createFramebuffer(VkRenderPass pass, const std::vector<VkImageView>& views,
                                       uint32_t w, uint32_t h, VkFramebuffer* out) {
    VkFramebufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    ci.renderPass = pass;
    ci.attachmentCount = uint32_t(views.size());
    ci.pAttachments = views.data();
    ci.width = w;
    ci.height = h;
    ci.layers = 1;
    return vkCreateFramebuffer(device_, &ci, nullptr, out) == VK_SUCCESS;
}

void VulkanRenderer::imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                                  VkImageAspectFlags aspect) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &b);
}

/** Builds the mip chain with blit chains - the kernel hands over mip level 0 only. */
void VulkanRenderer::generateMips(VkCommandBuffer cmd, VkImage image, int32_t width, int32_t height,
                                  uint32_t levels) {
    if (levels < 2) return;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    int32_t w = width, h = height;
    for (uint32_t i = 1; i < levels; ++i) {
        const int32_t pw = w, ph = h;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;

        b.subresourceRange.baseMipLevel = i - 1;
        b.subresourceRange.levelCount = 1;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
        blit.srcOffsets[1] = {pw, ph, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        blit.dstOffsets[1] = {w, h, 1};
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        b.subresourceRange.baseMipLevel = i;
        b.subresourceRange.levelCount = 1;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    }
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &b);
}

/* ============================================================================================
 *  Pipelines
 * ============================================================================================ */
bool VulkanRenderer::createPipelines(std::string* error) {
    VkDescriptorSetLayout sets[3] = {setLayout0_, setLayout1_, setLayout2_};
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = 3;
    li.pSetLayouts = sets;
    AF_VK_CHECK(vkCreatePipelineLayout(device_, &li, nullptr, &avatarLayout_));

    /* Forward+ uses the same camera block and storage-buffer set as the graphics path. */
    AF_VK_CHECK(vkCreatePipelineLayout(device_, &li, nullptr, &forwardPlusLayout_));
    {
        const AfShaderVk* cs = afShaderVk("forward_plus", AF_STAGE_COMP);
        if (!cs) { *error = "forward_plus.comp missing from the SPIR-V table"; return false; }
        VkShaderModule module = makeModule(cs);
        if (!module) { *error = "Forward+ compute module creation failed"; return false; }
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = "main";
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = stage;
        ci.layout = forwardPlusLayout_;
        const VkResult r = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr,
                                                     &forwardPlusPipeline_);
        vkDestroyShaderModule(device_, module, nullptr);
        if (r != VK_SUCCESS) { setError(error, "vkCreateComputePipelines(Forward+)", r); return false; }
    }

    li.setLayoutCount = 1;
    AF_VK_CHECK(vkCreatePipelineLayout(device_, &li, nullptr, &postLayout_));

    /* ---- shared state blocks ---- */
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState noBlend{};
    noBlend.blendEnable = VK_FALSE;
    noBlend.colorWriteMask = 0xF;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &noBlend;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depth.maxDepthBounds = 1.0f;

    const VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    /* ---- shadow pass: depth only, no fragment shader, slope-scaled bias ---- */
    {
        const AfShaderVk* vs = afShaderVk("shadow", AF_STAGE_VERT);
        if (!vs) { *error = "shadow.vert missing from the SPIR-V table"; return false; }
        VkShaderModule vert = makeModule(vs);
        if (vert == VK_NULL_HANDLE) { *error = "shadow vertex module creation failed"; return false; }

        VkVertexInputBindingDescription binding{0, VertexLayout::stride, VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attr{0, 0, VK_FORMAT_R32G32B32_SFLOAT, VertexLayout::posOffset};
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &binding;
        vin.vertexAttributeDescriptionCount = 1;
        vin.pVertexAttributeDescriptions = &attr;

        VkPipelineRasterizationStateCreateInfo sr = raster;
        sr.depthBiasEnable = VK_TRUE;
        sr.depthBiasConstantFactor = 1.25f;
        sr.depthBiasSlopeFactor = 1.75f;

        VkPipelineDepthStencilStateCreateInfo sd = depth;

        VkPipelineColorBlendStateCreateInfo sb = blend;
        sb.attachmentCount = 0;
        sb.pAttachments = nullptr;

        VkGraphicsPipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        stage.module = vert;
        stage.pName = "main";
        ci.stageCount = 1;
        ci.pStages = &stage;
        ci.pVertexInputState = &vin;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &sr;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &sd;
        ci.pColorBlendState = &sb;
        ci.pDynamicState = &dynamic;
        ci.layout = avatarLayout_;
        ci.renderPass = shadowPass_;
        const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &shadowPipeline_);
        vkDestroyShaderModule(device_, vert, nullptr);
        if (r != VK_SUCCESS) { setError(error, "vkCreateGraphicsPipelines(shadow)", r); return false; }
    }

    /* Full attribute set for the avatar and ground passes. */
    VkVertexInputBindingDescription bindings[2] = {
        {0, VertexLayout::stride, VK_VERTEX_INPUT_RATE_VERTEX},
        {0, sizeof(float) * 3, VK_VERTEX_INPUT_RATE_VERTEX},
    };
    VkVertexInputAttributeDescription avatarAttrs[5] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, VertexLayout::posOffset},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, VertexLayout::normalOffset},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, VertexLayout::tangentOffset},
        {3, 0, VK_FORMAT_R32G32_SFLOAT, VertexLayout::uvOffset},
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, VertexLayout::colorOffset},
    };

    /* ---- geometry pipeline (avatar) ---- */
    {
        const AfShaderVk* vs = afShaderVk("forward", AF_STAGE_VERT);
        const AfShaderVk* fs = afShaderVk("forward", AF_STAGE_FRAG);
        if (!vs || !fs) { *error = "forward shaders missing from the SPIR-V table"; return false; }
        VkShaderModule vert = makeModule(vs), frag = makeModule(fs);
        if (!vert || !frag) { *error = "forward shader module creation failed"; return false; }

        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &bindings[0];
        vin.vertexAttributeDescriptionCount = 5;
        vin.pVertexAttributeDescriptions = avatarAttrs;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vert; stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";

        VkPipelineMultisampleStateCreateInfo fm = ms;
        fm.rasterizationSamples = VkSampleCountFlagBits(options_.msaaSamples);

        VkGraphicsPipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vin;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &raster;
        ci.pMultisampleState = &fm;
        ci.pDepthStencilState = &depth;
        ci.pColorBlendState = &blend;
        ci.pDynamicState = &dynamic;
        ci.layout = avatarLayout_;
        ci.renderPass = hdrPass_;
        const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &forwardPipeline_);
        if (r != VK_SUCCESS) { setError(error, "vkCreateGraphicsPipelines(forward)", r); return false; }

        /* Ground: the studio plane never needs back-face culling, and it must stay visible when
           the camera dips below the horizon line. */
        VkPipelineVertexInputStateCreateInfo gvin = vin;
        gvin.vertexBindingDescriptionCount = 1;
        gvin.pVertexBindingDescriptions = &bindings[1];
        gvin.vertexAttributeDescriptionCount = 1;
        gvin.pVertexAttributeDescriptions = &avatarAttrs[0];
        VkPipelineRasterizationStateCreateInfo gr = raster;
        gr.cullMode = VK_CULL_MODE_NONE;
        ci.pVertexInputState = &gvin;
        ci.pRasterizationState = &gr;
        /* The ground has its own shader pair (as on the GLES path): it is a single plane with no
           vertex normal, its albedo comes from an analytic pattern instead of the atlas, and it
           must ignore the region inspector. Sharing the avatar's fragment shader here would mean
           feeding it undefined interpolants. */
        const AfShaderVk* gvs = afShaderVk("ground", AF_STAGE_VERT);
        const AfShaderVk* gfs = afShaderVk("ground", AF_STAGE_FRAG);
        if (!gvs || !gfs) {
            vkDestroyShaderModule(device_, vert, nullptr);
            vkDestroyShaderModule(device_, frag, nullptr);
            *error = "ground shaders missing from the SPIR-V table";
            return false;
        }
        VkShaderModule gvert = makeModule(gvs), gfrag = makeModule(gfs);
        if (!gvert || !gfrag) {
            vkDestroyShaderModule(device_, vert, nullptr);
            vkDestroyShaderModule(device_, frag, nullptr);
            *error = "ground shader module creation failed";
            return false;
        }
        stages[0].module = gvert;
        stages[1].module = gfrag;
        const VkResult r2 = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &groundPipeline_);
        vkDestroyShaderModule(device_, gvert, nullptr);
        vkDestroyShaderModule(device_, gfrag, nullptr);
        stages[0].module = vert;
        stages[1].module = frag;
        ci.pVertexInputState = &vin;
        ci.pRasterizationState = &raster;
        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
        if (r2 != VK_SUCCESS) { setError(error, "vkCreateGraphicsPipelines(ground)", r2); return false; }
    }

    /* ---- skeleton overlay ---- */
    {
        const AfShaderVk* vs = afShaderVk("line", AF_STAGE_VERT);
        const AfShaderVk* fs = afShaderVk("line", AF_STAGE_FRAG);
        if (!vs || !fs) { *error = "line shaders missing from the SPIR-V table"; return false; }
        VkShaderModule vert = makeModule(vs), frag = makeModule(fs);
        if (!vert || !frag) { *error = "line shader module creation failed"; return false; }

        VkVertexInputBindingDescription binding{0, sizeof(float) * 7, VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[2] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 3},
        };
        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &binding;
        vin.vertexAttributeDescriptionCount = 2;
        vin.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo lia = ia;
        lia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

        VkPipelineColorBlendAttachmentState lb{};
        lb.blendEnable = VK_TRUE;
        lb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        lb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        lb.colorBlendOp = VK_BLEND_OP_ADD;
        lb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        lb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        lb.alphaBlendOp = VK_BLEND_OP_ADD;
        lb.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo lcb = blend;
        lcb.pAttachments = &lb;

        VkPipelineDepthStencilStateCreateInfo ld = depth;
        ld.depthWriteEnable = VK_FALSE;   /* the overlay must not occlude what comes after it */

        VkPipelineRasterizationStateCreateInfo lr = raster;
        lr.cullMode = VK_CULL_MODE_NONE;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vert; stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";

        VkPipelineMultisampleStateCreateInfo lm = ms;
        lm.rasterizationSamples = VkSampleCountFlagBits(options_.msaaSamples);

        VkGraphicsPipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vin;
        ci.pInputAssemblyState = &lia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &lr;
        ci.pMultisampleState = &lm;
        ci.pDepthStencilState = &ld;
        ci.pColorBlendState = &lcb;
        ci.pDynamicState = &dynamic;
        ci.layout = postLayout_;
        ci.renderPass = hdrPass_;
        const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &linePipeline_);
        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
        if (r != VK_SUCCESS) { setError(error, "vkCreateGraphicsPipelines(line)", r); return false; }
    }

    /* ---- fullscreen passes: bloom down, bloom up, composite ---- */
    {
        const AfShaderVk* vs = afShaderVk("fullscreen", AF_STAGE_VERT);
        if (!vs) { *error = "fullscreen.vert missing from the SPIR-V table"; return false; }
        struct PostPass {
            const char* name;
            VkRenderPass pass;
            VkPipeline* pipeline;
            bool additive;
        };
        const PostPass passes[3] = {
            {"bloom_down", bloomDownPass_, &bloomDownPipeline_, false},
            {"bloom_up", bloomUpPass_, &bloomUpPipeline_, true},
            {"composite", compositePass_, &compositePipeline_, false},
        };
        for (const PostPass& p : passes) {
            const AfShaderVk* fs = afShaderVk(p.name, AF_STAGE_FRAG);
            if (!fs) { *error = "post shader missing from the SPIR-V table"; return false; }
            VkShaderModule vert = makeModule(vs), frag = makeModule(fs);
            if (!vert || !frag) { *error = "post shader module creation failed"; return false; }

            VkPipelineVertexInputStateCreateInfo vin{};
            vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

            VkPipelineRasterizationStateCreateInfo pr = raster;
            pr.cullMode = VK_CULL_MODE_NONE;

            VkPipelineDepthStencilStateCreateInfo pd{};
            pd.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            pd.depthTestEnable = VK_FALSE;
            pd.depthWriteEnable = VK_FALSE;
            pd.maxDepthBounds = 1.0f;

            VkPipelineColorBlendAttachmentState pb{};
            pb.colorWriteMask = 0xF;
            if (p.additive) {
                pb.blendEnable = VK_TRUE;
                pb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                pb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                pb.colorBlendOp = VK_BLEND_OP_ADD;
                pb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                pb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            }
            VkPipelineColorBlendStateCreateInfo pcb = blend;
            pcb.pAttachments = &pb;

            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vert; stages[0].pName = "main";
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";

            VkGraphicsPipelineCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            ci.stageCount = 2;
            ci.pStages = stages;
            ci.pVertexInputState = &vin;
            ci.pInputAssemblyState = &ia;
            ci.pViewportState = &vp;
            ci.pRasterizationState = &pr;
            ci.pMultisampleState = &ms;
            ci.pDepthStencilState = &pd;
            ci.pColorBlendState = &pcb;
            ci.pDynamicState = &dynamic;
            ci.layout = postLayout_;
            ci.renderPass = p.pass;
            const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, p.pipeline);
            vkDestroyShaderModule(device_, vert, nullptr);
            vkDestroyShaderModule(device_, frag, nullptr);
            if (r != VK_SUCCESS) { setError(error, "vkCreateGraphicsPipelines(post)", r); return false; }
        }
    }

    if (!createDescriptorPool(error)) return false;
    if (!writeDescriptorSets(error)) return false;
    return true;
}

/* ============================================================================================
 *  Descriptors
 * ============================================================================================ */
bool VulkanRenderer::createDescriptorPool(std::string* error) {
    const int sets = 1 /*forward*/ + 2 * bloomLevels_ + 2 /*composite, spare*/ + 1 /*Forward+*/;
    const int samplers = sets * 4 + 8;
    const int ubos = sets * 2 + 4;
    VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(samplers)},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, uint32_t(ubos)},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
    };
    VkDescriptorPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.maxSets = uint32_t(sets + 4);
    ci.poolSizeCount = 3;
    ci.pPoolSizes = sizes;
    AF_VK_CHECK(vkCreateDescriptorPool(device_, &ci, nullptr, &descriptorPool_));
    return true;
}

bool VulkanRenderer::writeDescriptorSets(std::string* error) {
    if (!descriptorPool_) return true;
    vkResetDescriptorPool(device_, descriptorPool_, 0);

    /* One descriptor set per post pass (each needs its own source image), plus one for the
       geometry pass and one for the material set. All of them carry the camera block (dynamic
       offset per frame in flight) and the post block (dynamic offset per pass). */
    const int forwardSetCount = 1;
    const int bloomDownCount = bloomLevels_ > 0 ? bloomLevels_ : 0;
    const int bloomUpCount = bloomLevels_ > 1 ? bloomLevels_ - 1 : 0;
    std::vector<VkDescriptorSetLayout> layouts(forwardSetCount + bloomDownCount + bloomUpCount + 1, setLayout0_);
    std::vector<VkDescriptorSet> sets(layouts.size(), VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = descriptorPool_;
    ai.descriptorSetCount = uint32_t(layouts.size());
    ai.pSetLayouts = layouts.data();
    AF_VK_CHECK(vkAllocateDescriptorSets(device_, &ai, sets.data()));

    int cursor = 0;
    VkDescriptorSet forwardSet = sets[cursor++];
    std::vector<VkDescriptorSet> bloomDown(bloomDownCount), bloomUp(bloomUpCount);
    for (int i = 0; i < bloomDownCount; ++i) bloomDown[i] = sets[cursor++];
    for (int i = 0; i < bloomUpCount; ++i) bloomUp[i] = sets[cursor++];
    VkDescriptorSet compositeSet = sets[cursor++];

    VkDescriptorSet materialSet = VK_NULL_HANDLE;
    {
        VkDescriptorSetAllocateInfo mai = ai;
        mai.descriptorSetCount = 1;
        mai.pSetLayouts = &setLayout1_;
        AF_VK_CHECK(vkAllocateDescriptorSets(device_, &mai, &materialSet));
    }
    materialSet_ = materialSet;
    cameraSets_.assign(1, forwardSet);
    lineSet_ = forwardSet;
    compositeSet_ = compositeSet;
    bloomDownSets_ = bloomDown;
    bloomUpSets_ = bloomUp;

    VkDescriptorSetAllocateInfo fpa = ai;
    fpa.descriptorSetCount = 1;
    fpa.pSetLayouts = &setLayout2_;
    AF_VK_CHECK(vkAllocateDescriptorSets(device_, &fpa, &forwardPlusSet_));

    VkDescriptorBufferInfo camInfo{cameraUbos_.empty() ? VK_NULL_HANDLE : cameraUbos_[0], 0, sizeof(CameraUbo)};
    VkDescriptorBufferInfo postInfo{postUbo_, 0, sizeof(PostUbo)};
    VkDescriptorImageInfo shadowInfo{shadowSampler_, shadowView_,
                                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo scratchInfo{postSampler_, hdrView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    auto writeSet = [&](VkDescriptorSet set, const VkDescriptorImageInfo& source,
                        const VkDescriptorImageInfo& bloom) {
        VkWriteDescriptorSet w[5]{};
        for (int i = 0; i < 5; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = uint32_t(i);
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w[0].pBufferInfo = &camInfo;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo = &shadowInfo;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w[2].pBufferInfo = &postInfo;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[3].pImageInfo = &source;
        w[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[4].pImageInfo = &bloom;
        vkUpdateDescriptorSets(device_, 5, w, 0, nullptr);
    };

    writeSet(forwardSet, scratchInfo, scratchInfo);
    for (int i = 0; i < bloomDownCount; ++i) {
        VkDescriptorImageInfo src = scratchInfo;
        src.imageView = (i == 0) ? hdrView_ : bloom_[i - 1].view;
        writeSet(bloomDown[i], src, scratchInfo);
    }
    for (int i = 0; i < bloomUpCount; ++i) {
        VkDescriptorImageInfo src = scratchInfo;
        src.imageView = bloom_[i + 1].view;   /* target = level i, source = level i + 1 */
        writeSet(bloomUp[i], src, scratchInfo);
    }
    {
        VkDescriptorImageInfo bloom = scratchInfo;
        if (bloomLevels_ > 0) bloom.imageView = bloom_[0].view;
        writeSet(compositeSet, scratchInfo, bloom);
    }

    /* Material set: dynamic uniform buffer + the five atlas maps. */
    {
        VkDescriptorBufferInfo matInfo{materialUbo_, 0, sizeof(MaterialUbo)};
        VkDescriptorImageInfo tex[5];
        for (int i = 0; i < 5; ++i) tex[i] = {atlasSampler_, atlasViews_[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[6]{};
        for (int i = 0; i < 6; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = materialSet;
            w[i].dstBinding = uint32_t(i);
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w[0].pBufferInfo = &matInfo;
        for (int i = 0; i < 5; ++i) {
            w[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i + 1].pImageInfo = &tex[i];
        }
        vkUpdateDescriptorSets(device_, 6, w, 0, nullptr);
    }
    {
        VkDescriptorBufferInfo lights{forwardLightsBuffer_, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo tiles{forwardTilesBuffer_, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[2]{};
        for (int i = 0; i < 2; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = forwardPlusSet_;
            w[i].dstBinding = uint32_t(i);
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        w[0].pBufferInfo = &lights;
        w[1].pBufferInfo = &tiles;
        vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    }
    (void)error;
    return true;
}

/* ============================================================================================
 *  Atlas upload
 * ============================================================================================ */
bool VulkanRenderer::createFallbackAtlas(std::string* error) {
    /* 1x1 opaque white, replaced by uploadAtlas() the moment the kernel has baked the real maps.
       The shader only samples a slot when the corresponding has_*_tex flag is set, so this is a
       descriptor-validity placeholder, not a stand-in for a missing feature. */
    /* A fresh image's contents are undefined, and an undefined value in a PBR multiply is not
       acceptable even when the shader is about to ignore it, so each placeholder is cleared to
       white with a one-shot command buffer. */
    VkCommandPool tmpPool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo poolCi{};
    poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCi.queueFamilyIndex = queueFamily_;
    if (commandPool_ == VK_NULL_HANDLE) vkCreateCommandPool(device_, &poolCi, nullptr, &tmpPool);

    for (int i = 0; i < AF_TEX_COUNT; ++i) {
        if (atlasViews_[i] != VK_NULL_HANDLE) continue;
        if (!createRenderTarget(1, 1, VK_FORMAT_R8_UNORM,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                &atlasImages_[i], &atlasMemory_[i], &atlasViews_[i],
                                VK_IMAGE_ASPECT_COLOR_BIT)) {
            *error = "atlas placeholder allocation failed";
            return false;
        }
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = tmpPool ? tmpPool : commandPool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(device_, &ai, &cmd);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        imageBarrier(cmd, atlasImages_[i], VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        VkClearColorValue white{};
        white.float32[0] = white.float32[1] = white.float32[2] = white.float32[3] = 1.0f;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, atlasImages_[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
        VkImageMemoryBarrier toShader{};
        toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toShader.srcQueueFamilyIndex = toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShader.image = atlasImages_[i];
        toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toShader.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toShader);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue_);
        vkFreeCommandBuffers(device_, ai.commandPool, 1, &cmd);
    }
    if (tmpPool) vkDestroyCommandPool(device_, tmpPool, nullptr);
    atlasWidth_ = 0;
    atlasHeight_ = 0;
    stats_.textureBytes = AF_TEX_COUNT * 4ull;
    return true;
}

bool VulkanRenderer::uploadAtlas(std::string* error) {
    if (!engine_) return true;
    uint64_t atlasBytes = 0;
    bool anyAtlas = false;

    const int levels[AF_TEX_COUNT] = {AF_TEX_BASE_COLOR, AF_TEX_NORMAL, AF_TEX_METALLIC_ROUGHNESS,
                                      AF_TEX_OCCLUSION, AF_TEX_SUBSURFACE};
    for (int slot = 0; slot < AF_TEX_COUNT; ++slot) {
        af_texture tex{};
        if (af_engine_texture(engine_, levels[slot], &tex) != AF_OK) continue;
        if (!tex.data || tex.width <= 0 || tex.height <= 0) continue;

        /* Format selection: base colour is sRGB-encoded in the atlas (it is the only map that
           carries perceptual colour), everything else is linear data. */
        VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
        if (tex.channels == 1) format = VK_FORMAT_R8_UNORM;
        else if (levels[slot] == AF_TEX_BASE_COLOR) format = VK_FORMAT_R8G8B8A8_SRGB;

        uint32_t mipLevels = 1;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(phys_, format, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) {
            uint32_t m = 1;
            int32_t mw = tex.width, mh = tex.height;
            while (mw > 1 || mh > 1) { mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1; ++m; }
            mipLevels = std::min(m, 12u);
        }

        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMem = VK_NULL_HANDLE;
        void* stagingMap = nullptr;
        if (!createBuffer(tex.byte_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &staging, &stagingMem, &stagingMap)) {
            *error = "atlas staging buffer failed";
            return false;
        }
        std::memcpy(stagingMap, tex.data, size_t(tex.byte_size));

        if (atlasViews_[slot] != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, atlasViews_[slot], nullptr);
            atlasViews_[slot] = VK_NULL_HANDLE;
        }
        if (atlasImages_[slot] != VK_NULL_HANDLE) {
            vkDestroyImage(device_, atlasImages_[slot], nullptr);
            atlasImages_[slot] = VK_NULL_HANDLE;
        }
        if (atlasMemory_[slot] != VK_NULL_HANDLE) {
            vkFreeMemory(device_, atlasMemory_[slot], nullptr);
            atlasMemory_[slot] = VK_NULL_HANDLE;
        }
        if (!createImage(uint32_t(tex.width), uint32_t(tex.height), format,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT, &atlasImages_[slot], &atlasMemory_[slot], mipLevels) ||
            !createView(atlasImages_[slot], format, VK_IMAGE_ASPECT_COLOR_BIT, &atlasViews_[slot], mipLevels)) {
            *error = "atlas image creation failed";
            return false;
        }
        VkMemoryRequirements atlasReq{};
        vkGetImageMemoryRequirements(device_, atlasImages_[slot], &atlasReq);
        atlasBytes += atlasReq.size;
        anyAtlas = true;

        /* One-time upload: transition, copy, build mips, hand over to the fragment shader. */
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        {
            VkCommandPoolCreateInfo pool{};
            pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool.queueFamilyIndex = queueFamily_;
            VkCommandPool tmp = VK_NULL_HANDLE;
            if (commandPool_ == VK_NULL_HANDLE) vkCreateCommandPool(device_, &pool, nullptr, &tmp);
            VkCommandBufferAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            ai.commandPool = tmp ? tmp : commandPool_;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            vkAllocateCommandBuffers(device_, &ai, &cmd);
            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &bi);

            imageBarrier(cmd, atlasImages_[slot], VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {uint32_t(tex.width), uint32_t(tex.height), 1};
            vkCmdCopyBufferToImage(cmd, staging, atlasImages_[slot],
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            generateMips(cmd, atlasImages_[slot], tex.width, tex.height, mipLevels);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
            vkQueueWaitIdle(queue_);
            vkFreeCommandBuffers(device_, ai.commandPool, 1, &cmd);
            if (tmp) vkDestroyCommandPool(device_, tmp, nullptr);
        }
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, stagingMem, nullptr);
        atlasWidth_ = tex.width;
        atlasHeight_ = tex.height;
    }
    atlasUploaded_ = true;
    stats_.textureBytes = anyAtlas ? atlasBytes : AF_TEX_COUNT * 4ull;
    /* The descriptor sets hold the image views, so they must be rewritten after an upload. */
    if (!writeDescriptorSets(error)) return false;
    return true;
}

/* ============================================================================================
 *  Per-frame state
 * ============================================================================================ */
void VulkanRenderer::updateScene(float deltaSeconds) {
    (void)deltaSeconds;
    /* Environment -> the analytic sky parameters -> 9 SH coefficients. The SH projection is the
       only per-change CPU cost in the lighting path (about 0.13 M sky evaluations) and it is
       skipped entirely while the environment is unchanged. */
    env_.sunAzimuthDeg = options_.sunAzimuthDeg;
    env_.sunElevationDeg = options_.sunElevationDeg;
    env_.sunIntensity = options_.sunIntensity;
    env_.turbidity = options_.turbidity;
    env_.groundAlbedo = options_.groundAlbedo;
    env_.intensity = options_.environmentIntensity;
    projectToSh(env_, &sh_, 1024);

    modelBounds(engine_, scene_.modelMin, scene_.modelMax, &scene_.hasModel);
    buildCameraUbo(&scene_, camera_, options_, env_, sh_, int(swapExtent_.width), int(swapExtent_.height), true);

    scene_.camera.envFlags[3] = flatShading_ ? 1.0f : 0.0f;
    scene_.camera.uiParams[0] = float(highlightRegion_);
    scene_.camera.uiParams[1] = highlightRegion_ >= 0 ? 1.0f : 0.0f;
    /* PCF radius in shadow-map UV units, so the shader never needs to know the map size. */
    scene_.camera.uiParams[2] = float(options_.shadowSoftness) / float(options_.shadowMapSize ? options_.shadowMapSize : 2048);
    scene_.camera.uiParams[3] = 1.0f / float(swapExtent_.height ? swapExtent_.height : 1);
    scene_.camera.envFlags[0] = options_.enableShadows ? 1.0f : 0.0f;

    if (!cameraUboMapped_.empty() && cameraUboMapped_[0])
        std::memcpy(static_cast<char*>(cameraUboMapped_[0]) + size_t(frameIndex_) * cameraStride_,
                    &scene_.camera, sizeof(CameraUbo));

    /* ---- post-process slices ------------------------------------------------------------------ */
    if (postUboMapped_) {
        auto slice = [&](uint32_t i) {
            return reinterpret_cast<PostUbo*>(static_cast<char*>(postUboMapped_) + size_t(i) * uboAlignment_);
        };
        /* On an 8-bit HDR target the sun disc and the eye highlight clip at 1.0, so the bloom
           threshold drops with them; on a float target they are found at their real intensity. */
        const float hdrThreshold = (hdrFormat_ == VK_FORMAT_R8G8B8A8_UNORM) ? 0.62f : 1.0f;
        for (int i = 0; i < bloomLevels_; ++i) {
            PostUbo* p = slice(uint32_t(i));
            p->params[0] = (i == 0) ? hdrThreshold : 0.0f;
            p->params[1] = 0.5f;
            p->params[2] = p->params[3] = 0.0f;
            const uint32_t sw = (i == 0) ? swapExtent_.width : bloom_[i - 1].width;
            const uint32_t sh = (i == 0) ? swapExtent_.height : bloom_[i - 1].height;
            p->texel[0] = 1.0f / float(sw ? sw : 1);
            p->texel[1] = 1.0f / float(sh ? sh : 1);
            p->texel[2] = p->texel[3] = 0.0f;
        }
        for (int j = 0; j + 1 < bloomLevels_; ++j) {
            PostUbo* p = slice(uint32_t(bloomLevels_ + j));
            p->params[0] = p->params[1] = p->params[2] = p->params[3] = 0.0f;
            /* The tent kernel is sized in the *target* (finer) texel: the upsample widens as it
               climbs the chain, which is what produces a single smooth glow. */
            p->texel[0] = 1.0f / float(bloom_[j].width ? bloom_[j].width : 1);
            p->texel[1] = 1.0f / float(bloom_[j].height ? bloom_[j].height : 1);
            p->texel[2] = p->texel[3] = 0.0f;
        }
        PostUbo* c = slice(uint32_t(2 * bloomLevels_));
        c->params[0] = 0.0f;
        c->params[1] = 0.0f;
        c->params[2] = options_.enableBloom ? 0.055f : 0.0f;
        c->params[3] = options_.exposure;
        c->texel[0] = 1.0f / float(swapExtent_.width ? swapExtent_.width : 1);
        c->texel[1] = 1.0f / float(swapExtent_.height ? swapExtent_.height : 1);
        c->texel[2] = c->texel[3] = 0.0f;
    }

    rebuildMaterialBlocks();
    buildSkeletonLines();
}

/** Writes one std140 material block per submesh, plus one extra block for the studio ground. */
void VulkanRenderer::rebuildMaterialBlocks() {
    if (!engine_ || !device_) return;
    const int submeshCount = af_engine_submesh_count(engine_);
    const af_submesh* submeshes = af_engine_submeshes(engine_);
    const af_material* materials = af_engine_materials(engine_);
    const int materialCount = af_engine_material_count(engine_);
    if (submeshCount <= 0 || !submeshes) { materialCount_ = 0; return; }

    const uint32_t stride = uboAlignment_ > sizeof(MaterialUbo) ? uboAlignment_
                                                                 : uint32_t(sizeof(MaterialUbo) + 15u) / 16u * 16u;
    materialStride_ = stride;
    const uint64_t needed = uint64_t(submeshCount + 1) * stride;
    if (needed > materialUboSize_) {
        /* Grow once and for all: the descriptor points at the buffer object, so it has to be
           rewritten, and that must not happen while a frame is still reading the old one. */
        vkDeviceWaitIdle(device_);
        if (materialUbo_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, materialUbo_, nullptr);
            vkFreeMemory(device_, materialUboMem_, nullptr);
            materialUbo_ = VK_NULL_HANDLE;
            materialUboMem_ = VK_NULL_HANDLE;
            materialUboMapped_ = nullptr;
        }
        const uint64_t capacity = needed * 2;
        if (!createBuffer(capacity, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &materialUbo_, &materialUboMem_, &materialUboMapped_)) {
            materialUboSize_ = 0;
            return;
        }
        materialUboSize_ = capacity;
        writeDescriptorSets(nullptr);
    }

    char* base = static_cast<char*>(materialUboMapped_);
    for (int i = 0; i < submeshCount; ++i) {
        const af_submesh& sm = submeshes[i];
        const af_material& m = (materials && sm.material_index >= 0 && sm.material_index < materialCount)
                                   ? materials[sm.material_index] : af_material{};
        MaterialUbo block{};
        block.baseColorFactor[0] = m.base_color_factor[0];
        block.baseColorFactor[1] = m.base_color_factor[1];
        block.baseColorFactor[2] = m.base_color_factor[2];
        block.baseColorFactor[3] = m.base_color_factor[3];
        block.pbr[0] = m.metallic;
        block.pbr[1] = m.roughness;
        block.pbr[2] = m.ior;
        block.pbr[3] = m.sheen;
        block.pbr2[0] = m.subsurface;
        block.pbr2[1] = m.anisotropic;
        block.pbr2[2] = float(materialKindForRegion(sm.region, m.metallic));
        block.pbr2[3] = m.has_normal_tex ? 1.0f : 0.0f;
        block.texFlags[0] = m.has_base_color_tex ? 1.0f : 0.0f;
        block.texFlags[1] = m.has_mr_tex ? 1.0f : 0.0f;
        block.texFlags[2] = m.has_ao_tex ? 1.0f : 0.0f;
        block.texFlags[3] = atlasUploaded_ ? 1.0f : 0.0f;
        block.region[0] = float(sm.region);
        block.region[1] = (highlightRegion_ >= 0 && sm.region == highlightRegion_) ? 1.0f : 0.0f;
        /* Eye regions additionally carry the uv extent of their atlas island plus a flag: the eye
           shader needs it to turn a refraction offset in metres into an atlas offset, so the iris
           parallax is right at whatever texture resolution the projector chose. */
        block.region[2] = irisUvExtent(engine_, sm);
        block.region[3] = (block.pbr2[2] == float(AF_KIND_EYE)) ? 1.0f : 0.0f;
        std::memcpy(base + size_t(i) * stride, &block, sizeof(block));
    }

    /* Ground block: a matte dielectric, its region set to -1 so the inspector highlight, which
       matches on region id, can never select it. */
    MaterialUbo ground{};
    ground.baseColorFactor[0] = ground.baseColorFactor[1] = ground.baseColorFactor[2] = 0.32f;
    ground.baseColorFactor[3] = 1.0f;
    ground.pbr[0] = 0.0f;
    ground.pbr[1] = 0.75f;
    ground.pbr[2] = 1.5f;
    ground.pbr[3] = 0.0f;
    ground.pbr2[2] = float(AF_KIND_GENERIC);
    ground.region[0] = -1.0f;
    std::memcpy(base + size_t(submeshCount) * stride, &ground, sizeof(ground));
    materialCount_ = submeshCount;
}

/** Builds the bone overlay: one line per joint to its parent, coloured by bone class. */
void VulkanRenderer::buildSkeletonLines() {
    lineVertexCount_ = 0;
    if (!engine_ || !skeletonVisible_ || !lineMapped_) return;
    const int J = af_engine_joint_count(engine_);
    if (J <= 0) return;
    std::vector<float> matrices(size_t(J) * 16);
    if (af_engine_joint_matrices(engine_, matrices.data(), J) != AF_OK) return;

    float* out = static_cast<float*>(lineMapped_);
    const uint32_t maxVerts = uint32_t(lineCapacity_ / (sizeof(float) * 7));
    uint32_t count = 0;
    for (int j = 0; j < J && count + 2 <= maxVerts; ++j) {
        af_joint_info info{};
        if (af_engine_joint_info(engine_, j, &info) != AF_OK) continue;
        if (info.parent < 0 || info.parent >= J) continue;

        /* Joint world matrices: the translation column is the joint's origin in world space. */
        const float* a = &matrices[size_t(j) * 16];
        const float* b = &matrices[size_t(info.parent) * 16];

        float rgb[3] = {0.35f, 0.72f, 1.00f};                       /* limb / torso */
        if (info.flags & AF_JOINT_FACE) rgb[0] = 1.00f, rgb[1] = 0.62f, rgb[2] = 0.18f;
        if (info.flags & AF_JOINT_FINGER) rgb[0] = 0.30f, rgb[1] = 1.00f, rgb[2] = 0.78f;
        if (info.flags & AF_JOINT_TWIST) rgb[0] = 0.78f, rgb[1] = 0.42f, rgb[2] = 1.00f;

        out[count * 7 + 0] = a[12]; out[count * 7 + 1] = a[13]; out[count * 7 + 2] = a[14];
        out[count * 7 + 3] = rgb[0]; out[count * 7 + 4] = rgb[1]; out[count * 7 + 5] = rgb[2];
        out[count * 7 + 6] = 1.0f;
        ++count;
        out[count * 7 + 0] = b[12]; out[count * 7 + 1] = b[13]; out[count * 7 + 2] = b[14];
        out[count * 7 + 3] = rgb[0]; out[count * 7 + 4] = rgb[1]; out[count * 7 + 5] = rgb[2];
        out[count * 7 + 6] = 0.65f;   /* the segment fades towards the parent */
        ++count;
    }
    lineVertexCount_ = count;
}

void VulkanRenderer::updateAnimation(float deltaSeconds) {
    if (!engine_ || clip_ < 0) return;
    const float duration = af_engine_clip_duration(engine_, clip_);
    if (duration <= 0.0f) return;
    clipTime_ += deltaSeconds * clipSpeed_;
    if (clipLoop_) {
        clipTime_ = std::fmod(clipTime_, duration);
        if (clipTime_ < 0.0f) clipTime_ += duration;
    } else if (clipTime_ > duration) {
        clipTime_ = duration;
    }
    if (af_engine_apply_clip(engine_, clip_, clipTime_) == AF_OK) meshDirty_ = true;
}

void VulkanRenderer::setClip(int clip, float speed, bool loop) {
    clip_ = clip;
    clipSpeed_ = speed;
    clipLoop_ = loop;
    clipTime_ = 0.0f;
    if (engine_ && clip_ >= 0 && af_engine_apply_clip(engine_, clip_, 0.0f) == AF_OK) meshDirty_ = true;
}

/** Uploads the kernel's (deformed) vertex stream and, when it changed, the index buffer. */
void VulkanRenderer::uploadDynamicGeometry(VkCommandBuffer cmd) {
    if (!engine_) return;
    const int32_t vcount = af_engine_vertex_count(engine_);
    const int32_t icount = af_engine_index_count(engine_);
    const af_vertex* verts = af_engine_vertices(engine_);
    const uint32_t* indices = af_engine_indices(engine_);
    if (vcount <= 0 || !verts) return;

    /* ---- vertex buffer (host visible, re-created only when the mesh grows) ---- */
    const uint64_t need = uint64_t(vcount) * VertexLayout::stride;
    if (need > vertexCapacity_) {
        if (vertexBuffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, vertexBuffer_, nullptr);
            vkFreeMemory(device_, vertexMem_, nullptr);
            vertexBuffer_ = VK_NULL_HANDLE;
            vertexMapped_ = nullptr;
        }
        const uint64_t capacity = need + need / 4;
        if (!createBuffer(capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &vertexBuffer_, &vertexMem_, &vertexMapped_)) {
            vertexCapacity_ = 0;
            return;
        }
        vertexCapacity_ = capacity;
    }
    if (vertexMapped_ && meshDirty_)
        std::memcpy(vertexMapped_, verts, size_t(need));
    vertexCount_ = vcount;

    /* ---- index buffer: only re-uploaded when the topology itself changed ---- */
    if (icount > 0 && indices) {
        const uint64_t ineed = uint64_t(icount) * sizeof(uint32_t);
        if (ineed > indexCapacity_ || indexCount_ != icount) {
            if (indexBuffer_ != VK_NULL_HANDLE && ineed > indexCapacity_) {
                vkDestroyBuffer(device_, indexBuffer_, nullptr);
                vkFreeMemory(device_, indexMem_, nullptr);
                indexBuffer_ = VK_NULL_HANDLE;
                indexMapped_ = nullptr;
            }
            if (indexBuffer_ == VK_NULL_HANDLE) {
                const uint64_t capacity = ineed + ineed / 4;
                if (!createBuffer(capacity, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  &indexBuffer_, &indexMem_, &indexMapped_)) {
                    indexCapacity_ = 0;
                    return;
                }
                indexCapacity_ = capacity;
            }
            if (indexMapped_) std::memcpy(indexMapped_, indices, size_t(ineed));
            indexCount_ = icount;
        }

        const int submeshCount = af_engine_submesh_count(engine_);
        const af_submesh* submeshes = af_engine_submeshes(engine_);
        const uint64_t indirectNeed = uint64_t(std::max(0, submeshCount)) *
                                      sizeof(VkDrawIndexedIndirectCommand);
        if (indirectNeed > 0 && submeshes) {
            if (indirectNeed > indirectCapacity_) {
                if (indirectBuffer_ != VK_NULL_HANDLE) {
                    vkDestroyBuffer(device_, indirectBuffer_, nullptr);
                    vkFreeMemory(device_, indirectMem_, nullptr);
                    indirectBuffer_ = VK_NULL_HANDLE;
                    indirectMapped_ = nullptr;
                }
                const uint64_t capacity = indirectNeed + indirectNeed / 2;
                if (!createBuffer(capacity, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  &indirectBuffer_, &indirectMem_, &indirectMapped_)) {
                    indirectCapacity_ = 0;
                    indirectCommandCount_ = 0;
                } else {
                    indirectCapacity_ = capacity;
                }
            }
            if (indirectMapped_ && indirectBuffer_ != VK_NULL_HANDLE) {
                auto* commands = static_cast<VkDrawIndexedIndirectCommand*>(indirectMapped_);
                for (int i = 0; i < submeshCount; ++i) {
                    commands[i].indexCount = uint32_t(std::max(0, submeshes[i].index_count));
                    commands[i].instanceCount = 1;
                    commands[i].firstIndex = uint32_t(std::max(0, submeshes[i].index_offset));
                    commands[i].vertexOffset = 0;
                    commands[i].firstInstance = 0;
                }
                indirectCommandCount_ = uint32_t(submeshCount);
            }
        }
    }

    /* The uploads above were plain CPU stores into host-coherent memory; the GPU needs to see
       them before the vertex fetch below, and a memory dependency is the Vulkan way to say so. */
    VkMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    bar.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                        VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &bar, 0, nullptr, 0, nullptr);
    meshDirty_ = false;
}

/* ============================================================================================
 *  Command recording
 * ============================================================================================ */
void VulkanRenderer::recordShadowPass(VkCommandBuffer cmd) {
    if (!options_.enableShadows || !engine_) return;
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.renderPass = shadowPass_;
    bi.framebuffer = shadowFramebuffer_;
    bi.renderArea.extent = {uint32_t(options_.shadowMapSize), uint32_t(options_.shadowMapSize)};
    bi.clearValueCount = 1;
    bi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);

    if (vertexCount_ > 0 && vertexBuffer_ != VK_NULL_HANDLE && indexCount_ > 0) {
        VkViewport vp{0.0f, 0.0f, float(options_.shadowMapSize), float(options_.shadowMapSize), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {uint32_t(options_.shadowMapSize), uint32_t(options_.shadowMapSize)}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_, &offset);
        vkCmdBindIndexBuffer(cmd, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);
        const VkDescriptorSet set = cameraSets_.empty() ? VK_NULL_HANDLE : cameraSets_[0];
        if (set != VK_NULL_HANDLE) {
            const uint32_t offsets[2] = {frameIndex_ * cameraStride_, uint32_t(2 * bloomLevels_) * uboAlignment_};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, avatarLayout_, 0, 1, &set, 2, offsets);
        }
        const af_submesh* submeshes = af_engine_submeshes(engine_);
        const int n = af_engine_submesh_count(engine_);
        if (indirectBuffer_ != VK_NULL_HANDLE && indirectCommandCount_ >= uint32_t(n)) {
            for (int i = 0; i < n; ++i)
                vkCmdDrawIndexedIndirect(cmd, indirectBuffer_,
                                         VkDeviceSize(i) * sizeof(VkDrawIndexedIndirectCommand), 1,
                                         sizeof(VkDrawIndexedIndirectCommand));
        } else {
            for (int i = 0; i < n; ++i)
                vkCmdDrawIndexed(cmd, uint32_t(submeshes[i].index_count), 1,
                                 uint32_t(submeshes[i].index_offset), 0, 0);
        }
    }
    vkCmdEndRenderPass(cmd);
}

void VulkanRenderer::recordForwardPass(VkCommandBuffer cmd, uint32_t imageIndex) {
    (void)imageIndex;
    VkClearValue clears[3]{};
    clears[0].color = {{0.055f, 0.075f, 0.11f, 1.0f}};   /* deep studio backdrop, linear light */
    clears[1].color = {{0.0f, 0.0f, 0.0f, 1.0f}};        /* MSAA resolve target: not loaded */
    clears[2].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.renderPass = hdrPass_;
    bi.framebuffer = hdrFramebuffer_;
    bi.renderArea.extent = swapExtent_;
    /* The API requires a clear value for the highest attachment index that uses LOAD_OP_CLEAR,
       which is the resolve attachment (index 1) when MSAA is on and the depth attachment
       (index 1) otherwise - so three values cover both layouts. */
    bi.clearValueCount = (options_.msaaSamples > 1) ? 3 : 2;
    bi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0.0f, 0.0f, float(swapExtent_.width), float(swapExtent_.height), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, swapExtent_};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    const VkDescriptorSet set0 = cameraSets_.empty() ? VK_NULL_HANDLE : cameraSets_[0];
    const uint32_t offsets0[2] = {frameIndex_ * cameraStride_, uint32_t(2 * bloomLevels_) * uboAlignment_};

    /* ---- studio ground ------------------------------------------------------------------------ */
    if (options_.groundPlane && engine_ && vertexCount_ > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, groundPipeline_);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &groundBuffer_, &offset);
        if (set0 != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, avatarLayout_, 0, 1, &set0, 2, offsets0);
            const uint32_t matOffset = uint32_t(materialCount_) * materialStride_;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, avatarLayout_, 1, 1, &materialSet_, 1, &matOffset);
            if (forwardPlusSet_ != VK_NULL_HANDLE)
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, avatarLayout_, 2, 1,
                                        &forwardPlusSet_, 0, nullptr);
        }
        vkCmdDraw(cmd, 4, 1, 0, 0);
        ++drawCallsThisFrame_;
    }

    /* ---- the avatar --------------------------------------------------------------------------- */
    if (engine_ && vertexCount_ > 0 && vertexBuffer_ != VK_NULL_HANDLE && indexCount_ > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, forwardPipeline_);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_, &offset);
        vkCmdBindIndexBuffer(cmd, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);
        if (set0 != VK_NULL_HANDLE)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, avatarLayout_, 0, 1, &set0, 2, offsets0);
        if (forwardPlusSet_ != VK_NULL_HANDLE)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, avatarLayout_, 2, 1,
                                    &forwardPlusSet_, 0, nullptr);

        const af_submesh* submeshes = af_engine_submeshes(engine_);
        const int n = af_engine_submesh_count(engine_);
        for (int i = 0; i < n; ++i) {
            if (materialSet_ != VK_NULL_HANDLE) {
                const uint32_t matOffset = uint32_t(i) * materialStride_;
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, avatarLayout_, 1, 1,
                                        &materialSet_, 1, &matOffset);
            }
            if (indirectBuffer_ != VK_NULL_HANDLE && indirectCommandCount_ > uint32_t(i)) {
                vkCmdDrawIndexedIndirect(cmd, indirectBuffer_,
                                         VkDeviceSize(i) * sizeof(VkDrawIndexedIndirectCommand), 1,
                                         sizeof(VkDrawIndexedIndirectCommand));
            } else {
                vkCmdDrawIndexed(cmd, uint32_t(submeshes[i].index_count), 1,
                                 uint32_t(submeshes[i].index_offset), 0, 0);
            }
            ++drawCallsThisFrame_;
        }
        trianglesThisFrame_ = indexCount_ / 3;
    }

    /* ---- skeleton overlay --------------------------------------------------------------------- */
    if (skeletonVisible_ && lineVertexCount_ >= 2 && lineBuffer_ != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, linePipeline_);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &lineBuffer_, &offset);
        if (set0 != VK_NULL_HANDLE)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 0, 1, &set0, 2, offsets0);
        vkCmdDraw(cmd, lineVertexCount_, 1, 0, 0);
        ++drawCallsThisFrame_;
    }
    vkCmdEndRenderPass(cmd);
}

void VulkanRenderer::recordCommands(VkCommandBuffer cmd, uint32_t imageIndex) {
    drawCallsThisFrame_ = 0;
    trianglesThisFrame_ = 0;

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    uploadDynamicGeometry(cmd);

    /* 0. Forward+ light culling. The compute shader writes one compact light list per screen tile;
       the explicit barrier keeps the result ordered before any fragment stage consumes it. */
    if (forwardPlusPipeline_ != VK_NULL_HANDLE && forwardPlusSet_ != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, forwardPlusPipeline_);
        const uint32_t cameraOffset = frameIndex_ * cameraStride_;
        VkDescriptorSet sets[3] = {cameraSets_[0], materialSet_, forwardPlusSet_};
        const uint32_t dynamicOffsets[2] = {cameraOffset, 0};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, forwardPlusLayout_, 0, 3,
                                sets, 2, dynamicOffsets);
        vkCmdDispatch(cmd, (kForwardTilesX + 7) / 8, (kForwardTilesY + 7) / 8, 1);
        VkBufferMemoryBarrier tileBarrier{};
        tileBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        tileBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        tileBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        tileBarrier.srcQueueFamilyIndex = tileBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        tileBarrier.buffer = forwardTilesBuffer_;
        tileBarrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 1,
                             &tileBarrier, 0, nullptr);
    }

    /* 1. shadow */
    recordShadowPass(cmd);

    /* 2. forward */
    recordForwardPass(cmd, imageIndex);

    /* 3. bloom: thresholded downsample chain, then additive tent upsample back up the chain */
    if (bloomLevels_ > 0) {
        VkViewport vp{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        for (int i = 0; i < bloomLevels_; ++i) {
            VkRenderPassBeginInfo rp{};
            rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rp.renderPass = bloomDownPass_;
            rp.framebuffer = bloom_[i].framebuffer;
            rp.renderArea.extent = {bloom_[i].width, bloom_[i].height};
            vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
            vp.width = float(bloom_[i].width);
            vp.height = float(bloom_[i].height);
            vkCmdSetViewport(cmd, 0, 1, &vp);
            VkRect2D sc{{0, 0}, {bloom_[i].width, bloom_[i].height}};
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomDownPipeline_);
            const uint32_t offsets[2] = {frameIndex_ * cameraStride_, uint32_t(i) * uboAlignment_};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 0, 1,
                                    &bloomDownSets_[i], 2, offsets);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);
            ++drawCallsThisFrame_;
        }
        for (int i = 0; i + 1 < bloomLevels_; ++i) {
            VkRenderPassBeginInfo rp{};
            rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rp.renderPass = bloomUpPass_;
            rp.framebuffer = bloom_[i].framebuffer;
            rp.renderArea.extent = {bloom_[i].width, bloom_[i].height};
            vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
            vp.width = float(bloom_[i].width);
            vp.height = float(bloom_[i].height);
            vkCmdSetViewport(cmd, 0, 1, &vp);
            VkRect2D sc{{0, 0}, {bloom_[i].width, bloom_[i].height}};
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomUpPipeline_);
            const uint32_t offsets[2] = {frameIndex_ * cameraStride_, uint32_t(bloomLevels_ + i) * uboAlignment_};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 0, 1,
                                    &bloomUpSets_[i], 2, offsets);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);
            ++drawCallsThisFrame_;
        }
    }

    /* 4. composite into the swapchain image */
    {
        VkRenderPassBeginInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass = compositePass_;
        rp.framebuffer = swapFramebuffers_[imageIndex];
        rp.renderArea.extent = swapExtent_;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vp{0.0f, 0.0f, float(swapExtent_.width), float(swapExtent_.height), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, swapExtent_};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipeline_);
        const uint32_t offsets[2] = {frameIndex_ * cameraStride_, uint32_t(2 * bloomLevels_) * uboAlignment_};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 0, 1,
                                &compositeSet_, 2, offsets);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        ++drawCallsThisFrame_;
    }

    vkEndCommandBuffer(cmd);
    stats_.drawCalls = drawCallsThisFrame_;
    stats_.triangles = trianglesThisFrame_;
    stats_.vertices = vertexCount_;
}

/* ============================================================================================
 *  Frame loop
 * ============================================================================================ */
void VulkanRenderer::renderFrame(float deltaSeconds) {
    frameError_.clear();
    if (frameFatal_) return;
    if (!device_ || swapchain_ == VK_NULL_HANDLE) {
        frameError_ = "Vulkan device or swapchain is unavailable";
        frameFatal_ = true;
        return;
    }
    ++stats_.frameAttempts;

    const auto fail = [this](VkResult result, const char* operation) {
        frameError_ = std::string(operation) + " failed (VkResult " + std::to_string(int(result)) + ")";
        frameFatal_ = true;
        std::fprintf(stderr, "AvatarForge Vulkan: %s\n", frameError_.c_str());
    };
    const auto recreateFromSurface = [this, &fail]() -> bool {
        VkSurfaceCapabilitiesKHR caps{};
        const VkResult capResult = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps);
        if (capResult != VK_SUCCESS) { fail(capResult, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"); return false; }
        int w = int(caps.currentExtent.width), h = int(caps.currentExtent.height);
        if (w <= 0 || h <= 0 || w == int(UINT32_MAX) || h == int(UINT32_MAX)) {
            w = int(swapExtent_.width); h = int(swapExtent_.height);
        }
        if (w <= 0 || h <= 0) { fail(VK_ERROR_INITIALIZATION_FAILED, "surface extent"); return false; }
        swapExtent_.width = 0;  /* force rebuild even when the extent is unchanged */
        std::string error;
        if (!resize(w, h)) {
            frameError_ = "swapchain recreation failed: " + error;
            frameFatal_ = true;
            return false;
        }
        framebufferResized_ = false;
        return true;
    };

    if (targetsDirty_) {
        applyOptionChanges();
        if (frameFatal_) return;
    }

    const VkResult waited = vkWaitForFences(device_, 1, &inFlight_[frameIndex_], VK_TRUE, UINT64_MAX);
    if (waited != VK_SUCCESS) { fail(waited, "vkWaitForFences"); return; }

    uint32_t imageIndex = 0;
    const VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                                   imageAvailable_[frameIndex_], VK_NULL_HANDLE, &imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateFromSurface();
        return;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        fail(acquired, "vkAcquireNextImageKHR");
        return;
    }
    if (imageIndex >= swapFramebuffers_.size() || imageIndex >= renderFinished_.size()) {
        fail(VK_ERROR_INITIALIZATION_FAILED, "acquired swapchain image index");
        return;
    }
    if (acquired == VK_SUBOPTIMAL_KHR) framebufferResized_ = true;

    const VkResult fenceReset = vkResetFences(device_, 1, &inFlight_[frameIndex_]);
    if (fenceReset != VK_SUCCESS) { fail(fenceReset, "vkResetFences"); return; }

    updateScene(deltaSeconds);
    updateAnimation(deltaSeconds);

    const VkResult commandReset = vkResetCommandBuffer(cmd_[frameIndex_], 0);
    if (commandReset != VK_SUCCESS) { fail(commandReset, "vkResetCommandBuffer"); return; }
    recordCommands(cmd_[frameIndex_], imageIndex);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable_[frameIndex_];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_[frameIndex_];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished_[imageIndex];
    const VkResult submitted = vkQueueSubmit(queue_, 1, &submit, inFlight_[frameIndex_]);
    if (submitted != VK_SUCCESS) { fail(submitted, "vkQueueSubmit"); return; }
    ++stats_.frameSubmissions;

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished_[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;
    const VkResult presented = vkQueuePresentKHR(queue_, &present);
    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;

    if (presented == VK_SUCCESS || presented == VK_SUBOPTIMAL_KHR) {
        ++stats_.presentRequestsAccepted;
        ++stats_.frames;
        if (deltaSeconds > 0.0f && deltaSeconds < 1.0f) {
            const float alpha = 0.06f;
            stats_.frameMs = stats_.frameMs * (1.0f - alpha) + deltaSeconds * 1000.0f * alpha;
            stats_.fps = stats_.frameMs > 0.01f ? 1000.0f / stats_.frameMs : 0.0f;
        }
        if (presented == VK_SUBOPTIMAL_KHR) framebufferResized_ = true;
    } else if (presented == VK_ERROR_OUT_OF_DATE_KHR) {
        framebufferResized_ = true;
    } else {
        fail(presented, "vkQueuePresentKHR");
        return;
    }

    if (framebufferResized_) {
        recreateFromSurface();
    }
}

void VulkanRenderer::applyOptionChanges() {
    targetsDirty_ = false;
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    const RendererOptions rollback = activeTargetOptions_;

    const auto rebuild = [&](std::string* error) {
        return createShadowTarget(error) && createHdrTargets(error) && createPipelines(error) &&
               writeDescriptorSets(error);
    };

    destroyTargets();
    destroyPipelines();
    std::string error;
    if (rebuild(&error)) {
        activeTargetOptions_ = options_;
        stats_.shadowMapSize = options_.enableShadows ? options_.shadowMapSize : 0;
        stats_.bloomLevels = options_.enableBloom ? bloomLevels_ : 0;
        return;
    }

    /* Tier changes are opportunistic. If one target or pipeline allocation fails, reconstruct the
       last known-good configuration immediately instead of presenting an empty swapchain forever. */
    destroyTargets();
    destroyPipelines();
    options_ = rollback;
    std::string rollbackError;
    if (rebuild(&rollbackError)) {
        activeTargetOptions_ = options_;
        stats_.shadowMapSize = options_.enableShadows ? options_.shadowMapSize : 0;
        stats_.bloomLevels = options_.enableBloom ? bloomLevels_ : 0;
    } else {
        frameError_ = "render-target rollback failed: " + rollbackError;
        frameFatal_ = true;
    }
}

void VulkanRenderer::setOptions(const RendererOptions& options) {
    RendererOptions normalized = options;
    /* Runtime AUTO steps can request a sample count the device did not expose. Clamp every change,
       not only init, or an upgrade from 2x to 4x can build mismatched colour/depth attachments. */
    if (phys_ != VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(phys_, &props);
        const VkSampleCountFlags supported =
            props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
        int chosen = 1;
        const int wanted = std::max(1, std::min(8, normalized.msaaSamples));
        for (int s : {8, 4, 2}) {
            if (wanted >= s && (supported & VkSampleCountFlagBits(s))) { chosen = s; break; }
        }
        normalized.msaaSamples = chosen;
    }
    const bool needsRebuild =
        normalized.msaaSamples != options_.msaaSamples ||
        (normalized.enableShadows != options_.enableShadows) ||
        (normalized.enableShadows && normalized.shadowMapSize != options_.shadowMapSize) ||
        (normalized.enableBloom != options_.enableBloom) ||
        (normalized.enableBloom && normalized.bloomLevels != options_.bloomLevels);
    options_ = normalized;
    if (needsRebuild) targetsDirty_ = true;
    stats_.shadowMapSize = options_.enableShadows ? options_.shadowMapSize : 0;
    stats_.bloomLevels = options_.enableBloom ? bloomLevels_ : 0;
}

void VulkanRenderer::attachEngine(af_engine* engine) {
    engine_ = engine;
    meshDirty_ = true;
    atlasUploaded_ = false;
    materialUboSize_ = 0;
    if (engine_ && device_) {
        std::string error;
        uploadAtlas(&error);
        rebuildMaterialBlocks();
    }
}

void VulkanRenderer::invalidateMesh() { meshDirty_ = true; }

void VulkanRenderer::setCamera(const CameraState& camera) { camera_ = camera; }

int VulkanRenderer::currentClip() const { return clip_; }
float VulkanRenderer::clipTime() const { return clipTime_; }

void VulkanRenderer::waitIdle() {
    if (device_) vkDeviceWaitIdle(device_);
}

bool VulkanRenderer::resize(int width, int height) {
    if (!device_ || width <= 0 || height <= 0) return false;
    if (swapExtent_.width == uint32_t(width) && swapExtent_.height == uint32_t(height)) return true;
    vkDeviceWaitIdle(device_);
    destroySwapchainResources();
    destroyTargets();
    destroyPipelines();
    std::string error;
    if (!createSwapchain(&error, width, height)) return false;
    if (!createShadowTarget(&error)) return false;
    if (!createHdrTargets(&error)) return false;
    if (!createPipelines(&error)) return false;
    if (!writeDescriptorSets(&error)) return false;
    activeTargetOptions_ = options_;
    return true;
}

/* ============================================================================================
 *  Teardown
 * ============================================================================================ */
void VulkanRenderer::destroySwapchainResources() {
    for (VkFramebuffer fb : swapFramebuffers_) if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
    swapFramebuffers_.clear();
    for (VkImageView v : swapViews_) if (v) vkDestroyImageView(device_, v, nullptr);
    swapViews_.clear();
    swapImages_.clear();
    if (compositePass_) { vkDestroyRenderPass(device_, compositePass_, nullptr); compositePass_ = VK_NULL_HANDLE; }
    if (swapchain_) { vkDestroySwapchainKHR(device_, swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE; }
}

void VulkanRenderer::destroyTargets() {
    for (int i = 0; i < kMaxBloomLevels; ++i) {
        BloomLevel& b = bloom_[i];
        if (b.framebuffer) { vkDestroyFramebuffer(device_, b.framebuffer, nullptr); b.framebuffer = VK_NULL_HANDLE; }
        if (b.view) { vkDestroyImageView(device_, b.view, nullptr); b.view = VK_NULL_HANDLE; }
        if (b.image) { vkDestroyImage(device_, b.image, nullptr); b.image = VK_NULL_HANDLE; }
        if (b.memory) { vkFreeMemory(device_, b.memory, nullptr); b.memory = VK_NULL_HANDLE; }
    }
    if (bloomDownPass_) { vkDestroyRenderPass(device_, bloomDownPass_, nullptr); bloomDownPass_ = VK_NULL_HANDLE; }
    if (bloomUpPass_) { vkDestroyRenderPass(device_, bloomUpPass_, nullptr); bloomUpPass_ = VK_NULL_HANDLE; }
    if (hdrFramebuffer_) { vkDestroyFramebuffer(device_, hdrFramebuffer_, nullptr); hdrFramebuffer_ = VK_NULL_HANDLE; }
    if (hdrPass_) { vkDestroyRenderPass(device_, hdrPass_, nullptr); hdrPass_ = VK_NULL_HANDLE; }
    if (shadowFramebuffer_) { vkDestroyFramebuffer(device_, shadowFramebuffer_, nullptr); shadowFramebuffer_ = VK_NULL_HANDLE; }
    if (shadowPass_) { vkDestroyRenderPass(device_, shadowPass_, nullptr); shadowPass_ = VK_NULL_HANDLE; }
    for (VkImageView* v : {&hdrView_, &msaaView_, &depthView_, &shadowView_})
        if (*v) { vkDestroyImageView(device_, *v, nullptr); *v = VK_NULL_HANDLE; }
    for (VkImage* i : {&hdrImage_, &msaaImage_, &depthImage_, &shadowImage_})
        if (*i) { vkDestroyImage(device_, *i, nullptr); *i = VK_NULL_HANDLE; }
    for (VkDeviceMemory* m : {&hdrMem_, &msaaMem_, &depthMem_, &shadowMem_})
        if (*m) { vkFreeMemory(device_, *m, nullptr); *m = VK_NULL_HANDLE; }
}

void VulkanRenderer::destroyPipelines() {
    for (VkPipeline* p : {&shadowPipeline_, &forwardPipeline_, &groundPipeline_, &linePipeline_,
                          &bloomDownPipeline_, &bloomUpPipeline_, &compositePipeline_,
                          &forwardPlusPipeline_})
        if (*p) { vkDestroyPipeline(device_, *p, nullptr); *p = VK_NULL_HANDLE; }
    if (descriptorPool_) { vkDestroyDescriptorPool(device_, descriptorPool_, nullptr); descriptorPool_ = VK_NULL_HANDLE; }
    cameraSets_.clear();
    bloomDownSets_.clear();
    bloomUpSets_.clear();
    materialSet_ = VK_NULL_HANDLE;
    compositeSet_ = VK_NULL_HANDLE;
    lineSet_ = VK_NULL_HANDLE;
    forwardPlusSet_ = VK_NULL_HANDLE;
}

void VulkanRenderer::destroyAll() {
    if (!device_) {
        if (instance_) vkDestroyInstance(instance_, nullptr);
        return;
    }
    vkDeviceWaitIdle(device_);

    destroySwapchainResources();
    destroyTargets();
    destroyPipelines();

    for (int i = 0; i < AF_TEX_COUNT; ++i) {
        if (atlasViews_[i]) vkDestroyImageView(device_, atlasViews_[i], nullptr);
        if (atlasImages_[i]) vkDestroyImage(device_, atlasImages_[i], nullptr);
        if (atlasMemory_[i]) vkFreeMemory(device_, atlasMemory_[i], nullptr);
    }
    for (VkBuffer* b : {&vertexBuffer_, &indexBuffer_, &indirectBuffer_, &materialUbo_,
                        &groundBuffer_, &lineBuffer_, &postUbo_, &forwardLightsBuffer_,
                        &forwardTilesBuffer_})
        if (*b) vkDestroyBuffer(device_, *b, nullptr);
    for (VkDeviceMemory* m : {&vertexMem_, &indexMem_, &indirectMem_, &materialUboMem_,
                              &groundMem_, &lineMem_, &postUboMem_, &forwardLightsMem_,
                              &forwardTilesMem_})
        if (*m) vkFreeMemory(device_, *m, nullptr);
    for (size_t i = 0; i < cameraUbos_.size(); ++i) {
        vkDestroyBuffer(device_, cameraUbos_[i], nullptr);
        vkFreeMemory(device_, cameraUboMem_[i], nullptr);
    }
    cameraUbos_.clear();
    cameraUboMem_.clear();

    if (atlasSampler_) vkDestroySampler(device_, atlasSampler_, nullptr);
    if (shadowSampler_) vkDestroySampler(device_, shadowSampler_, nullptr);
    if (postSampler_) vkDestroySampler(device_, postSampler_, nullptr);
    if (avatarLayout_) vkDestroyPipelineLayout(device_, avatarLayout_, nullptr);
    if (forwardPlusLayout_) vkDestroyPipelineLayout(device_, forwardPlusLayout_, nullptr);
    if (postLayout_) vkDestroyPipelineLayout(device_, postLayout_, nullptr);
    if (setLayout0_) vkDestroyDescriptorSetLayout(device_, setLayout0_, nullptr);
    if (setLayout1_) vkDestroyDescriptorSetLayout(device_, setLayout1_, nullptr);
    if (setLayout2_) vkDestroyDescriptorSetLayout(device_, setLayout2_, nullptr);

    for (VkSemaphore s : imageAvailable_) vkDestroySemaphore(device_, s, nullptr);
    for (VkSemaphore s : renderFinished_) vkDestroySemaphore(device_, s, nullptr);
    for (VkFence f : inFlight_) vkDestroyFence(device_, f, nullptr);
    if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);

    vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (instance_) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
}

VulkanRenderer::~VulkanRenderer() { destroyAll(); }

} /* namespace */

/* The backend's factory. It lives in this translation unit because VulkanRenderer is defined in
   an anonymous namespace - nothing outside needs the type, only the abstract Renderer interface. */
Renderer* createVulkanRenderer(ANativeWindow* window, const RendererOptions& options, std::string* error) {
    VulkanRenderer* r = new VulkanRenderer();
    if (!r->init(window, options, error)) {
        delete r;
        return nullptr;
    }
    return r;
}

} /* namespace af */
