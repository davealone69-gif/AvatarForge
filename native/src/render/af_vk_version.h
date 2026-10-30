/* Vulkan core-version policy for the opt-in renderer; no loader/header dependency here so
 * the host selector regression can exercise the same physical-device gate. The Vulkan API
 * ignores the patch number in VkApplicationInfo.apiVersion. A phone reporting 1.3.304
 * qualifies because it implements the 1.3 core, not because this app can require .304.
 */
#ifndef AF_VK_VERSION_H
#define AF_VK_VERSION_H

#include <cstdint>

namespace af {

constexpr uint32_t kRequestedVkMajor = 1;
constexpr uint32_t kRequestedVkMinor = 3;

constexpr bool supportsRequestedVulkanCore(uint32_t major, uint32_t minor) {
    return major > kRequestedVkMajor ||
           (major == kRequestedVkMajor && minor >= kRequestedVkMinor);
}

} // namespace af

#endif // AF_VK_VERSION_H
