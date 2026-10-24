#include "af_glb_read.h"

#include <cstddef>
#include <cstdint>
#include <string>

/* Build with Clang's libFuzzer + ASan + UBSan. Every input is treated as hostile. */
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (!data || size == 0) return 0;
    af::GlbAsset asset;
    std::string error;
    (void)af::importGlb(data, size, &asset, &error);
    return 0;
}
