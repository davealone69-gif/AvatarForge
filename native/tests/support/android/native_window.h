/* Host-only stand-in used by render_gl_tests.cpp. Never on the Android include path. */
#pragma once
#include <cstdint>
struct ANativeWindow { int width; int height; };
inline int32_t ANativeWindow_setBuffersGeometry(ANativeWindow*, int32_t, int32_t, int32_t) { return 0; }
