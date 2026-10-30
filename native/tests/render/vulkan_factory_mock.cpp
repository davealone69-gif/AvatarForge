/* Only the backend selector uses this double. It does NOT simulate Vulkan commands or pixels.
 * The production build links native/src/render/af_vk.cpp instead of this file. */
#include "af_render.h"

namespace af_test_vk {
int attempts = 0;
int destroys = 0;
bool initSucceeds = false;
void reset() { attempts = 0; destroys = 0; initSucceeds = false; }
}

namespace af {
namespace {
class VkFactoryDouble final : public Renderer {
public:
    ~VkFactoryDouble() override { ++af_test_vk::destroys; }
    bool resize(int, int) override { return true; }
    void attachEngine(af_engine*) override {}
    void invalidateMesh() override {}
    void setCamera(const CameraState&) override {}
    void setOptions(const RendererOptions& options) override { options_ = options; }
    RendererOptions options() const override { return options_; }
    void renderFrame(float) override {
        ++stats_.frameAttempts;
        ++stats_.frameSubmissions;
        ++stats_.presentRequestsAccepted;
        ++stats_.frames;
    }
    void setSkeletonVisible(bool) override {}
    void setFlatShading(bool) override {}
    void setDiagnosticRung(int rung) override { if (rung >= 0 && rung < 8) diagnosticRung_ = rung; }
    int diagnosticRung() const override { return diagnosticRung_; }
    void setHighlightRegion(int) override {}
    void setClip(int, float, bool) override {}
    int currentClip() const override { return -1; }
    float clipTime() const override { return 0; }
    void waitIdle() override {}
    RendererStats stats() const override { return stats_; }
    RendererInfo info() const override {
        RendererInfo info;
        info.backend = "vulkan";
        info.deviceName = "test-double (not a GPU)";
        return info;
    }
private:
    RendererOptions options_{};
    RendererStats stats_{};
    int diagnosticRung_ = 7;
};
}
Renderer* createVulkanRenderer(ANativeWindow*, const RendererOptions&, std::string* error) {
    ++af_test_vk::attempts;
    if (af_test_vk::initSucceeds) {
        if (error) error->clear();
        return new VkFactoryDouble();
    }
    if (error) *error = "vkCreateInstance failed (VkResult -3)";
    return nullptr;
}
} /* namespace af */
