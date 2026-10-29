/*
 * shadow.frag -- depth-only GLES pass. A linked program needs both stages; this one writes
 * no colour because the shadow target has no colour attachment.
 */
/* Minimal stage interface: no shared varyings or vertex attributes are consumed here. */
#define AF_VARY_MINIMAL 1
#include "common.glsl"

void main() {
    /* Intentionally empty: depth-only pass. */
}
