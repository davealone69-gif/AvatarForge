#include "af_vk_version.h"

#include <cstdio>
#include <string_view>

namespace {
int checks = 0;
int failures = 0;

void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", label);
    }
}
} // namespace

int main() {
    check(std::string_view(af::vk::kRequestedTargetLabel) == "1.3.304",
          "requested Vulkan target label remains distinct and exact");
    check(af::vk::kApplicationApiVersion == VK_MAKE_API_VERSION(0, 1, 3, 0),
          "VkApplicationInfo uses Vulkan 1.3 with patch zero");
    check(af::vk::supportsRequiredApi(VK_API_VERSION_1_3),
          "Vulkan 1.3.0 runtime satisfies the requested 1.3 core feature level");
    check(af::vk::supportsRequiredApi(VK_MAKE_API_VERSION(0, 1, 3, 304)),
          "Vulkan 1.3.304-reported runtime satisfies the requested core feature level");
    check(af::vk::supportsRequiredApi(VK_MAKE_API_VERSION(0, 1, 4, 0)),
          "a newer Vulkan 1.4 runtime satisfies the 1.3 core feature level");
    check(!af::vk::supportsRequiredApi(VK_MAKE_API_VERSION(0, 1, 2, 999)),
          "a Vulkan 1.2 runtime is rejected despite a large patch value");
    check(!af::vk::supportsRequiredApi(VK_MAKE_API_VERSION(1, 1, 3, 304)),
          "a nonzero API variant is not treated as the requested variant");

    std::printf("Vulkan API target: %d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
