/*
 * ============================================================================================
 *  AvatarForge :: af_align.h  --  tiny dependency-free alignment maths shared by both backends
 * ============================================================================================
 *
 *  Kept separate from af_render_common.h on purpose: that header carries the renderer's own
 *  Mat4/Vec3 types which collide with the kernel's af_math.h in a single translation unit, so
 *  host-side tests that already use kernel headers can still include this file.
 */
#ifndef AF_ALIGN_H
#define AF_ALIGN_H

#include <cstdint>

namespace af {

/* Round x up to the next multiple of alignment. Works for any positive alignment, not only
   powers of two, so it is safe for GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT and Vulkan's
   minUniformBufferOffsetAlignment whatever the driver reports. The GLES camera UBO is
   double-buffered: each slot must be at least sizeof(CameraUbo) (448) and a multiple of the
   alignment, otherwise the second slot overruns the buffer (AF-020). */
constexpr uint32_t afRoundUp(uint32_t x, uint32_t alignment) {
    return alignment ? ((x + alignment - 1u) / alignment) * alignment : x;
}

}  // namespace af

#endif  /* AF_ALIGN_H */
