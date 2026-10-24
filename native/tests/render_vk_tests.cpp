/* Execute the production Vulkan renderer with Khronos validation on Mesa/lavapipe. Only Android
   surface creation is adapted to an Xlib window, supplied by Xvfb in tools/test_renderer_vk.sh. */
#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <android/native_window.h>

static Display* g_display = nullptr;
static Window g_window = 0;
static VkDebugUtilsMessengerEXT g_messenger = VK_NULL_HANDLE;
static int g_errors = 0;
static uint32_t g_waitFenceCount = 0;

static VKAPI_ATTR VkBool32 VKAPI_CALL validationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    ++g_errors;
    std::fprintf(stderr, "VULKAN VALIDATION: %s\n", data->pMessage);
    return VK_FALSE;
}

static VkResult createTestInstance(const VkInstanceCreateInfo* requested,
                                   const VkAllocationCallbacks* allocator, VkInstance* instance) {
    VkInstanceCreateInfo ci = *requested;
    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
                               VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    ci.enabledExtensionCount = 3;
    ci.ppEnabledExtensionNames = extensions;
    ci.enabledLayerCount = 1;
    ci.ppEnabledLayerNames = layers;
    VkDebugUtilsMessengerCreateInfoEXT debug{};
    debug.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    debug.pfnUserCallback = validationMessage;
    VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT features{};
    features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    features.enabledValidationFeatureCount = 1;
    features.pEnabledValidationFeatures = &sync;
    debug.pNext = &features;
    ci.pNext = &debug;
    const VkResult result = vkCreateInstance(&ci, allocator, instance);
    if (result == VK_SUCCESS) {
        const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(*instance, "vkCreateDebugUtilsMessengerEXT"));
        debug.pNext = nullptr;
        if (create) create(*instance, &debug, nullptr, &g_messenger);
    }
    return result;
}

static void destroyTestInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    if (g_messenger) {
        const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        destroy(instance, g_messenger, nullptr);
        g_messenger = VK_NULL_HANDLE;
    }
    vkDestroyInstance(instance, allocator);
}

static VkResult createXlibSurface(VkInstance instance, const VkAndroidSurfaceCreateInfoKHR*,
                                  const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    VkXlibSurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    ci.dpy = g_display;
    ci.window = g_window;
    return vkCreateXlibSurfaceKHR(instance, &ci, allocator, surface);
}

static VkResult waitForFrameOwners(VkDevice device, uint32_t count, const VkFence* fences,
                                    VkBool32 all, uint64_t timeout) {
    g_waitFenceCount = all == VK_TRUE ? count : 0;
    return vkWaitForFences(device, count, fences, all, timeout);
}

#define vkWaitForFences waitForFrameOwners
#define vkCreateInstance createTestInstance
#define vkDestroyInstance destroyTestInstance
#define vkCreateAndroidSurfaceKHR createXlibSurface
#include "render_test_profile.h"
#define makeGpuProfile rendererTestProfile
#include "../src/render/af_vk.cpp"
#undef makeGpuProfile
#undef vkCreateAndroidSurfaceKHR
#undef vkDestroyInstance
#undef vkCreateInstance
#undef vkWaitForFences
#include "render_test_fixture.h"

static int g_checks = 0, g_failures = 0;
#define CHECK(condition, message) do { ++g_checks; if (!(condition)) { \
    ++g_failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); } } while (0)

static size_t redPixels() {
    XSync(g_display, False);
    XImage* image = XGetImage(g_display, g_window, 0, 0, kRenderTestWidth, kRenderTestHeight, AllPlanes, ZPixmap);
    if (!image) return 0;
    size_t red = 0;
    const char* dumpPath = std::getenv("AF_RENDER_TEST_FRAME");
    FILE* ppm = dumpPath ? std::fopen(dumpPath, "wb") : nullptr;
    if (ppm) std::fprintf(ppm, "P6\n%d %d\n255\n", kRenderTestWidth, kRenderTestHeight);
    for (int y = 0; y < kRenderTestHeight; ++y) for (int x = 0; x < kRenderTestWidth; ++x) {
        const unsigned long p = XGetPixel(image, x, y);
        const unsigned long r = (p & image->red_mask) >> 16;
        const unsigned long g = (p & image->green_mask) >> 8;
        const unsigned long b = p & image->blue_mask;
        if (r > 50 && r > g * 1.5 && r > b * 1.5) ++red;
        if (ppm) { const unsigned char rgb[3] = { (unsigned char)r, (unsigned char)g, (unsigned char)b }; std::fwrite(rgb, 1, 3, ppm); }
    }
    if (ppm) std::fclose(ppm);
    XDestroyImage(image);
    return red;
}

int main() {
    g_display = XOpenDisplay(nullptr);
    if (!g_display) { std::fprintf(stderr, "X display unavailable; use xvfb-run\n"); return 2; }
    g_window = XCreateSimpleWindow(g_display, DefaultRootWindow(g_display), 0, 0, kRenderTestWidth, kRenderTestHeight, 0, 0, 0);
    XMapWindow(g_display, g_window);
    XSync(g_display, False);
    ANativeWindow window{kRenderTestWidth, kRenderTestHeight};
    af::RendererOptions options;
    options.msaaSamples = 4;
    options.shadowMapSize = 64;
    options.enableBloom = false;
    options.groundPlane = false;
    std::string error;
    std::unique_ptr<af::Renderer> renderer(af::createVulkanRenderer(&window, options, &error));
    if (!renderer) { std::fprintf(stderr, "Vulkan init failed: %s\n", error.c_str()); return 1; }
    std::printf("Vulkan: %s; profile=%s\n", renderer->info().deviceName.c_str(),
                renderer->info().gpuProfile.c_str());
    CHECK(g_errors == 0, "initialization has no Vulkan validation errors");
    if (g_errors) return 1; // Do not execute invalid commands on the unfixed implementation.
    renderer->renderFrame(1.0f / 60.0f);
    renderer->waitIdle();
    CHECK(g_errors == 0, "empty scene frame has no validation errors");
    CHECK(renderer->stats().fps > 0, "an empty scene still produces live frames");
    af_spec spec;
    af_spec_init(&spec);
    af_engine* rawEngine = nullptr;
    CHECK(af_engine_create(&spec, nullptr, &rawEngine) == AF_OK, "create engine");
    std::unique_ptr<af_engine, decltype(&af_engine_destroy)> engine(rawEngine, af_engine_destroy);
    const auto glb = triangleGlb();
    CHECK(af_engine_import_glb_from_memory(engine.get(), glb.data(), glb.size()) == AF_OK, "import fixture");
    renderer->attachEngine(engine.get());
    CHECK(g_errors == 0, "attach engine has no Vulkan validation errors");
    if (g_errors) return 1;
    for (int i = 0; i < 4; ++i) {
        af::CameraState camera;
        camera.yawDeg += i * 3.0f;
        renderer->setCamera(camera);
        renderer->renderFrame(1.0f / 60.0f);
        renderer->waitIdle();
        CHECK(g_errors == 0, "alternating camera slices / draw has no validation errors");
        CHECK(redPixels() > 200, "Vulkan presents visible geometry, not just frame statistics");
    }
    for (int i = 0; i < 8; ++i) renderer->renderFrame(1.0f / 60.0f);
    renderer->waitIdle();
    CHECK(g_errors == 0, "consecutive submissions without per-frame CPU idle have no validation errors");
    CHECK(g_waitFenceCount >= 2, "mapped shared buffers wait for every in-flight owner");
    CHECK(redPixels() > 200, "consecutive submissions keep visible pixels");
    CHECK(renderer->stats().fps > 0 && renderer->stats().triangles == 1 && !renderer->stats().frameFatal,
          "live timing and actual mesh submissions, not a stopped backend");
    CHECK(!std::getenv("AF_RENDER_TEST_FULL_PROFILE") || renderer->stats().msaaSamples > 1,
          "full coverage mode really exercised multisample targets");
    options.enableBloom = true;
    options.bloomLevels = 5;
    options.groundPlane = true;
    renderer->setOptions(options);
    renderer->renderFrame(1.0f / 60.0f);
    renderer->waitIdle();
    CHECK(g_errors == 0, "target rebuild and bloom have no validation errors");
    CHECK(redPixels() > 200, "Vulkan geometry survives quality change");
    CHECK(!std::getenv("AF_RENDER_TEST_FULL_PROFILE") || renderer->stats().bloomLevels == 5,
          "full coverage mode really exercised five bloom levels");
    for (int i = 0; i < 8; ++i) renderer->renderFrame(1.0f / 60.0f);
    renderer->waitIdle();
    CHECK(g_errors == 0, "consecutive multisample/bloom submissions have no synchronization errors");
    CHECK(redPixels() > 200, "consecutive bloom submissions keep visible geometry");
    options.msaaSamples = 1;
    options.enableShadows = false;
    options.enableBloom = false;
    renderer->setOptions(options);
    renderer->invalidateMesh();
    renderer->renderFrame(1.0f / 60.0f);
    renderer->waitIdle();
    CHECK(g_errors == 0, "single-sample / no-shadow quality downgrade has no validation errors");
    CHECK(redPixels() > 200, "geometry survives returning to single-sample targets");
    XResizeWindow(g_display, g_window, 320, 448);
    XSync(g_display, False);
    CHECK(renderer->resize(320, 448), "resize rebuilds the actual swapchain and GPU targets");
    renderer->renderFrame(1.0f / 60.0f);
    renderer->waitIdle();
    CHECK(g_errors == 0, "resized swapchain frame has no validation errors");
    CHECK(redPixels() > 200, "geometry survives an actual window resize");
    for (int size : {4, 0, 1, 0}) {
        renderer->waitIdle();
        const auto replacement = triangleGlb(size);
        CHECK(af_engine_import_glb_from_memory(engine.get(), replacement.data(), replacement.size()) == AF_OK,
              "import replacement texture fixture");
        if (size > 0) {
            af_texture texture{};
            CHECK(af_engine_texture(engine.get(), AF_TEX_BASE_COLOR, &texture) == AF_OK &&
                  texture.width == size && texture.height == size,
                  "fixture has a real embedded base-colour texture");
        }
        renderer->attachEngine(engine.get());
        renderer->renderFrame(1.0f / 60.0f);
        renderer->waitIdle();
        CHECK(g_errors == 0, "real / 1x1 / absent texture reattachment has no validation errors");
        const size_t visible = redPixels();
        std::printf("texture reattach: size=%d, visible red pixels=%zu\n", size, visible);
        CHECK(visible > 200, "texture replacement keeps opaque geometry visible");
    }
    renderer.reset();
    CHECK(g_errors == 0, "teardown has no validation errors");
    XDestroyWindow(g_display, g_window);
    XCloseDisplay(g_display);
    std::printf("Vulkan renderer: %d checks, %d failure(s), %d validation error(s)\n",
                g_checks, g_failures, g_errors);
    return g_failures ? 1 : 0;
}
