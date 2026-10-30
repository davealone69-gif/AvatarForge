# Performance baseline

Status: **PARTIAL** — prior device reports are real user evidence, not a current-head benchmark.

## Device evidence already collected (build 6883243, SM-S921B / Exynos 2400 class)

- API 37, arm64, kernel 0.10.1 ABI 1, RAM 7.1 GB, budget 1536 MB, thermal status severe.
- Report A: gles31 via ANGLE/Xclipse 940, 27.8 FPS / 35.92 ms, 417,150 submitted triangles, 15 draws, textures 90.7 MiB, quality 0, MSAA 1x, shadow 1024, bloom 0, two backend fallback records.
- Report B: 49.2 FPS / 20.32 ms, 300,184 submitted triangles, 15 draws, same texture accounting, four fallback records.
- These prove texture accounting and fallback journaling appeared on device. They do **not** prove sustained thermals, GPU time, memory stability, or current-head camera behaviour.

## Current measurement truth

- `frameMs`/FPS are render-loop intervals derived from monotonic time, not GPU timestamps. The first callback and the first callback after a reset are unmeasured and display as warm-up; no synthetic 60 Hz sample is reported.
- `cpuMs` is CPU wall time inside the backend frame call; it remains unknown until a real sample is available.
- `gpuMs` is unknown (-1); no timestamp query is implemented.
- Triangles are submitted index counts, not occlusion-culled visible triangles.
- Thermal severe now caps quality to LOW and pacing to ~30 FPS in source; device effect is unverified.

## Current renderer target and measurement scope

The current default is native Vulkan with explicit GLES and bounded GLES fallback. AvatarForge now
gates Vulkan startup on loader and physical-device API 1.3 and compiles/validates shaders for
`vulkan1.3`; host selector/version tests and the ARM64 native build do not execute Vulkan commands or
prove pixels. `VkApplicationInfo` encodes API 1.3 with patch zero; the requested label `1.3.304`, NDK
header revision 1.3.237, runtime loader version and physical-device API are separate values. The
supplied SM-S921B checker report is not an AvatarForge runtime measurement. No current-head FPS,
thermal, memory, or sharpness claim is made without an exact APK installed and measured on the phone.

## Required next measurements

1. Matched before/after camera screenshots and diagnostics on SM-S921B.
2. 10-minute viewer soak: FPS interval, CPU submit, thermal status, PSS/native heap, texture bytes, fallback count.
3. Diagnostic scene ladder to separate clear/submit/present/geometry/shader failures.
5. Matched pixel comparison for moiré prefilter; no visual superiority claim until then.
