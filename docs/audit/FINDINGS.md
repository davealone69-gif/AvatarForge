# Findings register

Labels: **REAL + VERIFIED** = evidence in this repo/environment; **PARTIAL** = implemented or observed but missing a required proof; **UNVERIFIED** = no execution proof; **MISSING** = absent; **BLOCKED** = external credential/device/tool required.

| ID | Severity | Finding | Status | Evidence / action |
|---|---|---|---|---|
| AF-001 | Critical | Release silently fell back to debug keystore; no AAB/provenance gate | PARTIAL | `app/build.gradle.kts` now fail-closed; CI builds explicitly unsigned AAB for compile proof; signed tag job requires secrets. CI/device signature proof pending |
| AF-002 | Critical | Vulkan frame loop ignored fatal VkResults and counted frames before present | PARTIAL | `af_vk.cpp` records `frameError_`, fatal halt, attempts/submissions/accepted presents; JNI throws into bounded recovery. GLES host fault test passes; Vulkan device unverified |
| AF-003 | High | FPS/frameMs were presented as GPU-side truth; cpuMs never measured | PARTIAL | Stats renamed semantically; `gpuMs=-1` unknown; BudgetRenderer measures CPU wall time; UI report labels interval/CPU/unknown GPU |
| AF-004 | High | Severe thermal device reports did not constrain renderer | PARTIAL | `RenderThermalPolicy` + API-29 monitor cap quality and pace to 30 FPS at severe; JVM tests added; device thermal behaviour unverified |
| AF-005 | Medium | Renderer settings lost on process death | PARTIAL | `SettingsStore`/codec added and wired; JVM tests added; Android persistence device proof pending |
| AF-006 | High | Overall progress added weight×fraction on every callback, overcounting stages | PARTIAL | `GenerationProgress` computes prior completed stages + current fraction; JVM tests added |
| AF-007 | Critical | `release()` could destroy native handle while jobs ran; job IDs/list not concurrency-safe | PARTIAL | Single engine dispatcher, atomic IDs, completed-job pruning, release waits for cancellation before destroy. Cross-thread renderer/kernel mutation fence still MISSING |
| AF-008 | High | Manifest advertised GLB VIEW target with no import handler | PARTIAL | Dead filter removed; native import limits added (128 MiB GLB, 16 MiB JSON, 2 M verts, PNG caps). User-facing import still MISSING |
| AF-009 | High | Shader embedder could overwrite SPIR-V with an empty table when glslc missing | PARTIAL | Embedder preserves existing table and fails if absent/empty; CI compiles shaders and diffs generated headers |
| AF-010 | High | Camera drag/pinch/panel fix not proven on device | UNVERIFIED | CI-green source fix exists; require SM-S921B retest with before/after diagnostics |
| AF-011 | Medium | Moiré band-limit lacks pixel comparison | UNVERIFIED | Bake-time prefilter implemented earlier; no matched before/after device pixels |
| AF-012 | High | Repository has no LICENSE | MISSING | GitHub metadata license null; do not assume reuse rights |
| AF-013 | Medium | Rust workspace version/URL stale and not built here | UNVERIFIED | `rust/Cargo.toml` v0.9.0 vs app 0.10.1; repository `example.invalid`; no rustc |
| AF-014 | Medium | Tracked `prebuilt/` and `artifacts/` can drift from source | PARTIAL | CI rebuilds ARM64 from checkout and hashes outputs; historical binaries still need provenance triage |
| AF-015 | Low | Recovery advisor can report old Vulkan fallback after GLES chosen | MISSING | Journal is historical; advisor needs current-session scoping |
| AF-016 | High | No GPU timestamps, sustained thermal/memory soak, or diagnostic scene ladder | MISSING | Performance baseline is short device reports only; ladder is planned, not implemented |
