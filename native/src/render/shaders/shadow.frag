/*
 * shadow.frag -- required by the GLES backend (a program needs both stages) and never used by the
 * Vulkan path (the shadow pipeline there is vertex-only). It writes nothing: the shadow target has
 * no colour attachment, which is why the GLES draw call sets glColorMask(false x4) around it.
 */
/* Minimal stage interface: these programs never read the shared varyings, and a Vulkan
   pipeline must be able to provide every vertex input its shaders consume. */
#define AF_VARY_MINIMAL 1
#include "common.glsl"

void main() {
    /* Intentionally empty: depth-only pass. */
}
