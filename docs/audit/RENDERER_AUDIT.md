# Renderer audit

Evidence: `native/src/render/*`, `sdk/.../render/*`, the 2026-10-24 fix docs, and one
on-device diagnostics report + screenshot (SM-S921B). No GPU is available in the audit
sandbox; all runtime statements cite device-supplied evidence or remain explicitly UNVERIFIED.

## 1. End-to-end frame path (traced)

```
user opens viewer
 → AndroidView factory: RenderSurfaceView created, holder callback registered
 → surfaceCreated        → diagnostics INITIALIZING, attemptCreate()
 → AvatarRenderer.create → Renderer::create (af_render.cpp): Vulkan first, GLES fallback
                           → BudgetRenderer wraps backend (quality ladder)
 → engine attach         → uploadAtlas() if baked maps exist (staging → image → mip chain)
 → frameLoop (HandlerThread, 60 Hz budget)
 → setCamera → renderFrame: scene UBO → shadow pass → forward passes → post/bloom → swapchain
 → stats() → RendererStats flow; first frame promotes diagnostics to RUNNING
 → surfaceDestroyed      → synchronous renderer destroy (2 s latch), WAITING_FOR_SURFACE
```

Each transition above was read in source; the only device-confirmed segment is
`attempt 1 Vulkan OK (495 ms) → RUNNING, 58 FPS, 14 draw calls, 426,170 triangles` from the
user's report. Avatar *submission* (vertices actually uploaded for the generated engine) is
therefore PARTIAL: one generated avatar has rendered on one device; sustained correctness is
unverified.

## 2. Vulkan audit (static)

| Area | Finding | Status |
|---|---|---|
| Instance/device/queue | single queue family, VK_KHR_swapchain only, feature probe before enable | REAL + VERIFIED (static) |
| Physical selection | first suitable device; adequate for single-GPU phones | acceptable |
| Swapchain | composite-alpha fallback list (`af_vk.cpp:612`), recreate on resize | REAL + VERIFIED (static) |
| Depth format | best-first probe D32→D24S8→D32S8→D16 with required feature flags; chosen name now in diagnostics | REAL + VERIFIED (static); device value UNVERIFIED until next report |
| Image layouts/barriers | mip-chain spec violations found and fixed (AF-010); second full-barrier pass recommended | PARTIAL |
| Synchronization | per-frame fences/semaphores with `kFramesInFlight=2`; no timeline semaphores | REAL + VERIFIED (static); stress UNVERIFIED |
| Descriptors | rebuilt after every atlas upload; layout count derived from sampled images | REAL + VERIFIED (static) |
| Pipelines | pre-baked SPIR-V headers; centralized `createPipelines` | REAL + VERIFIED (static) |
| MSAA | clamped to device sample counts | REAL + VERIFIED (static) |
| Device loss | no explicit `VK_ERROR_DEVICE_LOST` recovery path; surfaces as a thrown frame error → FAILED card + Retry | PARTIAL (recoverable manually, no automatic device-lost branch) |
| Resource destruction | explicit teardown in `release()`; swapchain/depth/HDR ordering present | REAL + VERIFIED (static) |

## 3. OpenGL ES audit (static)

| Area | Finding | Status |
|---|---|---|
| EGL context | explicit `eglBindAPI`, `ANativeWindow_setBuffersGeometry(nativeVisual)` fixes (2026-10-21/24) | REAL + VERIFIED (static) |
| Surface/loss | context-loss handling relies on the bounded create retry; no separate EGL_CONTEXT_LOST branch | PARTIAL |
| Mipmaps | driver `glGenerateMipmap` — no manual layout risk | REAL + VERIFIED (static) |
| Shaders | GLSL ES text headers, program checks | REAL + VERIFIED (static) |
| Device coverage | UNVERIFIED on the target device (the one device report used Vulkan) | UNVERIFIED |

ANGLE must not be asserted: the device report shows native Vulkan; whether GLES is routed via
ANGLE on this Samsung build has no evidence either way.

## 4. The "zero render / disappeared avatar" question

The directive lists zero FPS/draw/triangle and mid-session disappearance. Evidence so far:

- Telemetry cannot report fabricated zeros post-fix: `qualityLevel` defaults to tier 2, but FPS
  is only emitted from actual `r.stats()` after frames; the header says "waiting for the first
  frame" until `rendererInfo != null`.
- Known historical causes are now closed: EGL native-visual mismatch, hard-coded depth format,
  no-retry create, surface-size swapchain churn, mip corruption (see renderer fix docs).
- Still unverified: long-duration runs, background/foreground cycles, device loss under
  thermal pressure, and repeated engine re-attach. These need the device test matrix.
- The frame loop stops on any thrown frame exception (by design) and surfaces FAILED + Retry;
  no silent 0-FPS spinning path remains in the reviewed code.

## 5. Diagnostic scene / mode (directive Phase Five)

What exists today vs the requested staged scene:

| Requested stage | Status today | Gap |
|---|---|---|
| Clear-color-only frame | MISSING | needs a native test mode flag through the renderer C ABI |
| Triangle / static indexed mesh | MISSING | same |
| Textured / lit / skinned / blendshape meshes | production renderer draws these for a generated avatar, but no isolated fixtures | deterministic fixtures needed |
| Production avatar | REAL (one device run) | sustained/regression evidence missing |
| Counters (draw calls, triangles, quality tier, surface churn, attempts) | REAL in `RendererDiagnostics`/`RendererStats` | GPU-time and presented-frame counts unavailable without extension work |
| Explicit "unknown" states | REAL (null flows render "not started"; thermal null says unavailable) | — |

Recommended implementation (not started — it changes the renderer ABI and cannot be verified
without a device): add an `af_renderer_set_diagnostic_stage(int)` C entry that swaps the scene
between the eight stages without touching the engine, and drive it from a hidden Diagnostics
action. The existing `RendererDiagnostics` flow is the transport for its checkpoints.

## 6. Confirmed renderer defects and their state

1. Mip-chain layout violation → FIXED (`bac2168` + prebuilt `e9bc49b`); NDK compile green;
   visual retest UNVERIFIED (AF-010).
2. Depth format assumption → FIXED earlier; chosen format now visible in the Backend row.
3. No bounded recovery → FIXED (3-attempt sequence + Retry).
4. Red-card strobe / redundant resizes / zeroed counters → FIXED (`9b0af6d`, `bac2168`).
5. Quality-tier changes invisible → PARTIAL fix this session (transition events in
   diagnostics, no ABI change); ADPF remains planned (AF-008).
6. White head cap → UNVERIFIED hypothesis; retest after mip fix (AF-014).

## 7. Renderer verdict

- Static correctness: substantially improved; the two spec-level defects found in audits are
  closed with CI compile evidence.
- Runtime evidence: one successful Vulkan session on one device. Everything else — GLES path,
  sustained stability, loss recovery, visual fidelity after the mip fix — stays UNVERIFIED and
  is queued in TEST_MATRIX with exact steps.
