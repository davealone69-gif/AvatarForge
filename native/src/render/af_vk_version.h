#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

namespace af::vk {

/*
 * The requested target label is Vulkan 1.3.304. Vulkan's VkApplicationInfo::apiVersion
 * uses only the variant/major/minor feature level; Khronos specifies that its patch field is
 * ignored for instance creation. Encode the API target as 1.3.0 and keep the requested label
 * separate from NDK header revision, loader version, driver version and device API version.
 */
constexpr char kRequestedTargetLabel[] = "1.3.304";
constexpr uint32_t kApplicationApiVersion = VK_API_VERSION_1_3;
constexpr uint32_t kRequiredCoreApiVersion = VK_API_VERSION_1_3;

constexpr bool supportsRequiredApi(uint32_t version) noexcept {
    const uint32_t variant = VK_API_VERSION_VARIANT(version);
    const uint32_t major = VK_API_VERSION_MAJOR(version);
    const uint32_t minor = VK_API_VERSION_MINOR(version);
    return variant == 0 &&
           (major > 1 || (major == 1 && minor >= 3));
}

static_assert(kApplicationApiVersion == VK_MAKE_API_VERSION(0, 1, 3, 0),
              "VkApplicationInfo must request the 1.3 feature level with patch zero");
static_assert(VK_API_VERSION_MAJOR(kRequiredCoreApiVersion) == 1 &&
              VK_API_VERSION_MINOR(kRequiredCoreApiVersion) == 3,
              "Vulkan core target drift");

} // namespace af::vk
