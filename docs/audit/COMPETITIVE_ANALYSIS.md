# Competitive analysis and technology benchmarking

Scope: technologies relevant to an Android-first, offline 3D avatar engine. All external claims
cite sources fetched this session; no benchmark numbers are invented, and no claim is made that
AvatarForge outperforms any listed system without a repeatable comparison.

## 1. Candidate technologies

### 1.1 Google Filament

- What it is: mobile-first real-time PBR engine; backends include OpenGL ES 3.0+, Vulkan 1.0 and
  WebGPU on Android, plus Metal/desktop; ships `gltfio-android`, a glTF 2.0 loader, and a runtime
  material compiler [1](https://github.com/google/filament).
- Architecture: public/private split with a command-stream frontend and driver backend thread,
  entity-component managers, memory arenas; clustered forward, Cook-Torrance BRDF, IBL, tone
  mappers (AgX/ACES/PBR Neutral), TAA/FXAA/MSAA, DoF, bloom [2](https://deepwiki.com/google/filament/2-engine-architecture)
  [3](https://github.com/google/filament).
- License: Apache-2.0; Maven distribution (`com.google.android.filament:filament-android`,
  actively released, 1.74.x observed) [4](https://libraries.io/maven/com.google.android.filament:filament-android).
- Fit for AvatarForge:
  - Adopt/direct-use candidates: glTF import correctness (`gltfio`), material/IBL maturity,
    FrameGraph-style render passes.
  - Costs of adoption: replaces the custom renderer that now has on-device Vulkan evidence,
    the bounded-recovery diagnostics contract, and the ABI-controlled native surface; adds a
    large binary and a runtime shader compiler (`filamat`). The project's own 2026-10-18
    decision to stay custom remains technically defensible.
  - Recommendation: **adapt, don't adopt** — use Filament as the reference for PBR/IBL and
    glTF edge cases; benchmark before any swap (see §4).

### 1.2 bgfx

- What it is: cross-platform "bring your own engine" rendering abstraction over Vulkan,
  OpenGL ES 3.1+, Metal, D3D, WebGPU etc.; Android API 14+; BSD-2-Clause [5](https://github.com/bkaradzic/bgfx)
  [6](https://bkaradzic.github.io/bgfx/overview.html).
- Fit: deliberately stops at draw calls/buffers/shaders — no scene graph, no asset pipeline,
  no animation [7](https://resources.proceduralpixels.com/resource/r009/). AvatarForge already
  owns those layers, and its renderer is thin by design. The abstraction tax (normalised state
  model, custom shader toolchain, pinned revision upgrades [8](https://hysenlabs.com/projects/bkaradzic-bgfx))
  outweighs the benefit for a single-platform, two-backend app.
- Recommendation: **deliberately avoid** as a dependency; mine its backend-selection and
  resource-lifetime patterns when hardening Vulkan/GLES parity.

### 1.3 Android ADPF (Adaptive Performance / thermal)

- What it is: Thermal API (status + `getThermalHeadroom` prediction), Performance Hint Manager
  (API 31+), Game Mode/State APIs [9](https://developer.android.com/games/optimize/adpf).
- Operational constraints: headroom must not be polled more than once per 10 s or from multiple
  threads (returns NaN); NaN headroom means the API is unavailable; hint sessions report
  actual-vs-target work duration per frame [10](https://developer.android.com/games/optimize/adpf/thermal)
  [11](https://github.com/android/games-samples/tree/main/agdk/adpf).
- Fit: AvatarForge currently uses only `PowerManager.currentThermalStatus` and a frame-time
  ladder (`BudgetRenderer`). ADPF adds headroom-based *early* adaptation and CPU scheduling
  hints, with safe fallbacks on API < 31.
- Recommendation: **adopt in a planned native/Kotlin service** behind the existing
  `DeviceResourceGovernor` boundary; the diagnostics report must distinguish "thermals OK"
  from "headroom API unavailable" (today's null-thermal handling already does the latter).

### 1.4 glTF 2.0 ecosystem and Blender workflows

- The native writer/reader already targets glTF 2.0 binary; the host validator confirms accessor
  bounds, morph targets and skins on the sample (52 morphs, 71 joints, 15 animations).
- Blender remains the correct external reference for rigging/animation conventions and
  export-fidelity comparisons, not an embedded dependency. Unsupported extensions must remain
  explicit (the reader already emits warnings instead of failing silently).
- Recommendation: **benchmark before adoption** of any loader change; keep the in-tree reader as
  the offline guarantee.

### 1.5 Mobile rendering techniques (research themes, not libraries)

- GPU-driven/indirect draws and Forward+: real wins only when draw/light counts dominate; the
  current scene is one avatar, ~14 draw calls, 16 forward lights — measured overhead does not
  justify the complexity today. Revisit if multi-avatar scenes land.
- GPU skinning: skinning is CPU-side and the mesh is updated per frame; at ~305k vertices this
  is a candidate optimisation, but it changes the ABI and must be measured against the current
  17.3 ms device frame. **Benchmark before adoption.**
- ASTC/ETC2 texture compression: STATUS.md records this as not started; it trades bake-time
  complexity for GPU memory/bandwidth — the most concrete next asset-pipeline upgrade.

## 2. Side-by-side decision matrix

| Capability | AvatarForge today | Filament | bgfx | Action |
|---|---|---|---|---|
| Vulkan on Android | custom, one device-verified frame path | mature, broad device coverage | yes | keep custom; add device matrix |
| GLES fallback | custom 3.1 with EGL recovery | GLES 3.0+ default path | yes | keep; parity-test against ANGLE cases |
| glTF import | reader exists, not wired (AF-015) | gltfio, production-grade | none | wire own reader first; compare with gltfio fixtures |
| PBR/IBL | analytic sky + SH, SSS approximation | full IBL/material system | n/a | close specific gaps via reference, not rewrite |
| Adaptive quality | frame-time ladder, hysteresis | platform-agnostic hooks | none | add ADPF headroom/hints (planned) |
| Offline/no-INTERNET | enforced by permission + CI | works offline once vendored | works offline | current guarantee is stronger operationally |
| Binary surface | 3 small .so, static STL | larger multi-artifact Maven set | single abstraction + backends | keep small footprint |
| License | proprietary repo terms | Apache-2.0 | BSD-2 | both compatible if ever vendored |

## 3. What not to do

- No dependency is introduced for popularity: every candidate above either replaces a working,
  smaller component or requires a measured bottleneck that does not yet exist.
- No comparative performance claim is made: a fair bake-off needs fixed scenes, fixed resolution,
  identical devices and documented thermal start conditions (PERFORMANCE_BASELINE defines the
  protocol; no such data exists yet).

## 4. Benchmark protocol before any renderer decision

1. Same device (SM-S921B class), same OS, airplane mode, 10-min cool-down, fixed performance mode
   where available.
2. Identical scene (the 417,150-triangle armor avatar), same resolution and camera path.
3. Warm-up 30 s; measure 120 s; record median/p95/p99 frame time, dropped frames, RSS, thermal
   status at start/end, quality-tier transitions.
4. Repeat 3×; report all runs, never a best-of.
5. A swap is justified only on a pre-registered, repeated difference — never on one run.

## Sources

1. [google/filament GitHub README](https://github.com/google/filament)
2. [Filament engine architecture — DeepWiki](https://deepwiki.com/google/filament/2-engine-architecture)
3. [Filament graphics backend abstraction — DeepWiki](https://deepwiki.com/google/filament/5-graphics-backend-abstraction)
4. [filament-android on Libraries.io](https://libraries.io/maven/com.google.android.filament:filament-android)
5. [bkaradzic/bgfx GitHub README](https://github.com/bkaradzic/bgfx)
6. [bgfx overview documentation](https://bkaradzic.github.io/bgfx/overview.html)
7. [bgfx knowledge-base summary](https://resources.proceduralpixels.com/resource/r009/)
8. [bgfx adoption constraints analysis](https://hysenlabs.com/projects/bkaradzic-bgfx)
9. [Android Dynamic Performance Framework overview](https://developer.android.com/games/optimize/adpf)
10. [ADPF Thermal API guidance and limits](https://developer.android.com/games/optimize/adpf/thermal)
11. [ADPF official sample (games-samples)](https://github.com/android/games-samples/tree/main/agdk/adpf)
