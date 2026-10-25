# Performance baseline

Rule applied: only measured numbers are quoted. There is no benchmark harness, no second device
sample and no controlled thermal protocol yet, so this file establishes the baseline *protocol*
and records the single real data point without extrapolating it into a claim.

## 1. The only measured device data point (user-supplied, 2026-10-24)

| Metric | Value | Source |
|---|---|---|
| Device | Samsung SM-S921B (Galaxy S24), Exynos 2400, Xclipse 940 (AMD RDNA-class) | diagnostics report |
| OS / API | Android 17, API 37 | report |
| Backend | Vulkan 1.3.304, driver 104869891 | RendererInfo |
| Frame rate | 58 FPS, 17.3 ms frame | `RendererStats` at tier 2 |
| Workload | 14 draw calls, 426,170 triangles | stats row |
| Memory | 7.1 GB total, 1,589 MB available at report time | governor snapshot |
| Thermal | none (PowerManager status) | governor snapshot |
| Start-up | attempt 1 Vulkan OK in 495 ms, no fallback needed | attempts card |

Status: **REAL + VERIFIED as a single observation**. Not a baseline: no warm-up control, no
duration, no distribution, no thermal start condition, one scene. A screenshot from the same
session shows surface artifacts (since addressed by the mip fix), so visual quality at this
frame rate is also not yet re-verified.

## 2. Adaptation logic as implemented (code-derived, not measured)

From `native/src/render/af_render.cpp`, `BudgetRenderer`:

- Warm-up gate: no adaptation before 45 frames; paused/background frames
  (`delta <= 0 || delta > 0.5 s`) are ignored.
- Smoothing: EMA with 0.9/0.1 weighting on frame delta.
- Step down: smoothed delta > 33.3 ms for 30 consecutive frames (~0.6 s at 60 Hz), one tier at
  a time, floor tier 0.
- Step up: smoothed delta < 16.6 ms for 240 frames (~4 s), AUTO preset only, ceiling tier 3.
- Only MSAA sample count, shadow map size and bloom level count change (4 fixed tiers);
  exposure, colour and geometry never change silently.
- This asymmetry is hysteresis against oscillation — consistent with the directive's
  requirement; what was missing was observability (AF-008, partially fixed: tier transitions
  now land in the diagnostics report).

Gap: the controller reacts to *total* frame time; CPU-vs-GPU attribution does not exist
(no timestamp-query pipeline). `cpuMs` is populated but GPU time is not measured.

## 3. Thermal integration (code-derived)

- `DeviceResourceGovernor` polls `PowerManager.currentThermalStatus`; MODERATE → WARN,
  SEVERE+ → FAIL in diagnostics; generation deferral uses the same snapshot.
- No `getThermalHeadroom`, no `PerformanceHintManager`, no Game Mode API (ADPF absent,
  COMPETITIVE_ANALYSIS §1.3). Google guidance caps headroom polling at 1 per 10 s and treats
  NaN as "unavailable" — the design constraints for the planned integration.
- Null thermal (pre-API 29 or unsupported) is reported as unavailable, never as safe.

## 4. Frame pacing (code-derived)

- The render thread posts the next frame for the unused portion of a 16.67 ms budget rather
  than sleeping a fixed 16 ms after work (fixed 2026-10-24); long frames therefore do not
  stack extra delay. No Choreographer/`AImageReader` pacing or swap interval tuning exists;
  pacing quality on 120 Hz panels is UNVERIFIED.

## 5. Baseline protocol for future measurements (to become real evidence)

1. Fixed scene: the armor avatar (417,150 triangles), fixed camera loop, tier AUTO, Vulkan and
   GLES runs separately.
2. Device: SM-S921B plus at least one Mali-class and one Adreno-class device; airplane mode;
   cooled to THERMAL_STATUS_NONE; fixed-performance mode where available.
3. Run: 30 s warm-up (discarded), 120 s measured, 3 repetitions per configuration.
4. Capture per second: FPS, median/p95/p99 frame ms, tier transitions (now in diagnostics),
   RSS, thermal status at start/end, surface recreate count.
5. Report all repetitions; never publish a best-of. Mark every device-less number as UNVERIFIED.

## 6. Performance findings register

| ID | Sev | Finding | Status |
|---|---|---|---|
| PERF-1 | medium | no GPU-time measurement; adaptation cannot distinguish CPU/GPU bound | CONFIRMED, planned |
| PERF-2 | medium | no ADPF headroom/hints; thermal reaction is status-only | CONFIRMED, planned |
| PERF-3 | low | tier changes were invisible; now in diagnostics but no long-run samples | PARTIAL |
| PERF-4 | low | 120 Hz panel pacing unverified | UNVERIFIED |
| PERF-5 | low | CPU skinning at ~305k vertices unmeasured against GPU-skin alternatives | UNVERIFIED; benchmark required before any ABI change |
| PERF-6 | info | single 58 FPS observation exists; no controlled benchmark | evidence recorded, baseline protocol defined |
