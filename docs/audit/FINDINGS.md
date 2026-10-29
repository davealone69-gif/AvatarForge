# Findings register

Labels: **REAL + VERIFIED** = evidence in this repo/environment; **PARTIAL** = implemented or observed but missing a required proof; **UNVERIFIED** = no execution proof; **MISSING** = absent; **BLOCKED** = external credential/device/tool required.

| ID | Severity | Finding | Status | Evidence / action |
|---|---|---|---|---|
| AF-001 | Critical | Release silently fell back to debug keystore; no AAB/provenance gate | PARTIAL | Fail-closed signing + CI unsigned AAB/provenance verified in run 40858597544; production signature still BLOCKED without credentials |
| AF-002 | Critical | Vulkan frame loop ignored fatal VkResults and counted frames before present | REAL + VERIFIED (removed) | User directed Vulkan removal. `af_vk.cpp`, SPIR-V table, Vulkan preference/fallback and Vulkan link dependency are deleted; GLES-only factory/recovery and binary linkage guard are CI-verified. Historical Vulkan defect is no longer a forward target |
| AF-003 | High | FPS/frameMs were presented as GPU-side truth; cpuMs never measured | PARTIAL | Stats renamed semantically; `gpuMs=-1` unknown; BudgetRenderer measures CPU wall time; UI report labels interval/CPU/unknown GPU |
| AF-004 | High | Severe thermal device reports did not constrain renderer | PARTIAL | `RenderThermalPolicy` + API-29 monitor cap quality and pace to 30 FPS at severe; JVM tests added; device thermal behaviour unverified |
| AF-005 | Medium | Renderer settings lost on process death | PARTIAL | `SettingsStore`/codec added and wired; JVM tests added; Android persistence device proof pending |
| AF-006 | High | Overall progress added weight×fraction on every callback, overcounting stages | PARTIAL | `GenerationProgress` computes prior completed stages + current fraction; JVM tests added |
| AF-007 | Critical | `release()` could destroy native handle while jobs ran; job IDs/list not concurrency-safe | PARTIAL | Single engine dispatcher, atomic IDs, completed-job pruning, release waits for cancellation before destroy. Cross-thread renderer/kernel mutation fence still MISSING |
| AF-008 | High | Manifest advertised GLB VIEW target with no import handler | PARTIAL | Dead filter removed; native import limits added (128 MiB GLB, 16 MiB JSON, 2 M verts, PNG caps). User-facing import still MISSING |
| AF-009 | High | Shader embedder could overwrite SPIR-V with an empty table when glslc missing | REAL + VERIFIED (removed) | SPIR-V path removed. GLES embedder requires the exact 12-stage set; `--check` requires glslc and rejects a stale table. CI compile/diff gate passes |
| AF-010 | High | Camera drag/pinch/panel fix not proven on device | PARTIAL | User confirms camera works. Preserve the current gesture path. Exact build SHA/counters and repeatable device regression report still missing |
| AF-011 | Medium | Moiré band-limit lacks pixel comparison | UNVERIFIED | Bake-time prefilter implemented earlier; no matched before/after device pixels |
| AF-012 | High | Repository has no LICENSE | MISSING | GitHub metadata license null; do not assume reuse rights |
| AF-013 | Medium | Rust workspace version/URL stale and not built here | UNVERIFIED | `rust/Cargo.toml` v0.9.0 vs app 0.10.1; repository `example.invalid`; no rustc |
| AF-014 | Medium | Tracked `prebuilt/` and `artifacts/` can drift from source | REAL + VERIFIED (mitigated) | Stale Vulkan-era prebuilt/APK/AAR/sample GLB removed from tracking; CI exact-head artifacts are authoritative and a linker guard rejects Vulkan dependencies |
| AF-015 | Low | Recovery advisor can report old Vulkan fallback after GLES chosen | PARTIAL | GLES-only builds never emit BACKEND_FALLBACK; old records remain decode-only and advisor labels them historical. Current-session scoping still needs device proof |
| AF-016 | High | No GPU timestamps, sustained thermal/memory soak, or diagnostic scene ladder | MISSING | Performance baseline is short device reports only; ladder is planned, not implemented |

| AF-017 | High | Visible model defects: world-axis palm blocks and skin poke-through on garments | PARTIAL | Hand palm now uses the anatomic hand frame; finger blend reduced; garment minimum layer raised to two lattice cells; lower-garment arm cut no longer removes thigh cloth. Focused host regressions pass. Device pixel confirmation pending |
| AF-018 | Medium | Viewer header covered the face/hands in device captures | PARTIAL | Header is compact by default with expandable build/driver details; camera gestures unchanged. Device visual confirmation pending |
| AF-019 | High | Current device GLB had swapped eye material sides, a 1,637-vs-156 eye triangle imbalance, a solid metal ball chest plate and a ponytail 7.7 cm above the crown | PARTIAL | `eyeballShape()` now uses anatomic sides and a hard union; red-green midpoints are reclassified; the cuirass is a torso-profile shell; hair bulk follows the cranium and carves explicit orbital clearance. Local export: 2,359/3,148 eye tris, plate front −0.139 m, hair top +0.024 m. Device pixels pending |
