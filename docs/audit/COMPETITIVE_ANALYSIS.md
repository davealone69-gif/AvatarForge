# Competitive / prior-art analysis

No superiority claim is made. This is an engineering adoption analysis from authoritative project sources retrieved 2026-10-29.

| Technology | Source / license | Activity observed | Relevant capability | Decision |
|---|---|---|---|---|
| Filament | https://github.com/google/filament — Apache-2.0 | pushed 2026-10-29, active | Android PBR, glTF, IBL, postprocess, mature renderer | **Benchmark/adapt ideas**; do not replace custom renderer without a measured migration plan |
| bgfx | https://github.com/bkaradzic/bgfx — BSD-2-Clause | pushed 2026-10-29, active | Cross-API rendering abstraction, Vulkan/GLES/Metal/D3D | **Adapt API-abstraction ideas**; avoid wholesale dependency until frame-error/telemetry contract is stable |
| Lugdunum | https://github.com/Lugdunum3D/Lugdunum — MIT | description says UNMAINTAINED, pushed 2018-05-29 | Vulkan/glTF C++14 engine experiments | **Avoid runtime adoption**; read only for historical design ideas |
| glTF 2.0 | https://github.com/KhronosGroup/glTF — spec CC-BY-4.0 | pushed 2026-10-28 | Asset container, accessors, skins, morphs, PBR materials | **Adopt spec conformance**; current validator/importer must be tested against malformed assets |
| meshoptimizer | https://github.com/zeux/meshoptimizer — MIT | pushed 2026-10-14 | Index/vertex optimization, simplification, meshlets | **Benchmark** for avatar LOD/overdraw; do not assume FPS gain without device measurement |
| ozz-animation | https://github.com/guillaumeblanc/ozz-animation — MIT | pushed 2026-08-01 | Skeletal animation sampling/blending | **Benchmark/adapt** if current clip system shows defects; preserve current ABI until measured |
| KTX-Software | https://github.com/KhronosGroup/KTX-Software — Apache-2.0 with exceptions | pushed 2026-10-29 | GPU texture compression/Basis/KTX2 | **Adopt later** for memory/bandwidth; requires toolchain and device format proof |
| Godot | https://github.com/godotengine/godot — MIT | pushed 2026-10-28 | Full engine, mobile renderer, animation/UI | **Avoid wholesale replacement**; useful comparison for diagnostics and mobile quality scaling |

## Applied conclusions

- The immediate competitive gap is not shader sophistication; it is **truthful lifecycle/telemetry/release integrity**. Filament/bgfx-class engines expose device loss and frame failure; AvatarForge's Vulkan path previously swallowed them. This patch adds frame-error propagation and distinct attempt/submission/present counters.
- Thermal pressure is a first-class mobile constraint. The prior-art lesson is to cap optional cost deterministically, not to claim a fixed FPS target. This patch adds a severe-thermal LOW-quality/30-FPS pacing cap.
- Texture compression and mesh optimization are promising next steps, but adopting them before fixing frame accounting would hide the real bottleneck. They are benchmark candidates, not unmeasured upgrades.
