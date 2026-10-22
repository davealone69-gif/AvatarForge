# AvatarForge branch integration audit — 2026-10-22

This is a selective integration, not a blind merge. Branch histories are substantially divergent; merging their histories wholesale would reintroduce old source and conflict with the active Android engine.

## Target line

- Current recovery line: `fix/renderer-frame-loop-resize-recovery`
- Staging line: `integrate/audited-branch-work`, based on the recovery line
- Final integration target remains `work/android-engine-dev` through the renderer recovery PR after CI and review.

## Branch-by-branch disposition

| Branch | Review result | Disposition |
|---|---|---|
| `fix/restore-self-test-menu-phase4` | Restores self-test/diagnostics menu routing in `MainActivity.kt`. | Integrated into recovery line; menu trigger changed to ⚙ SETTINGS. |
| `fix/diagnostics-invalid-render-telemetry` | Prevents zero/invalid FPS telemetry being presented as verified adaptation. | Integrated into recovery line. |
| `fix/avatar-head-hands-geometry-pass1` | Small incremental procedural head/hand geometry changes in `af_anatomy.cpp` and `af_field.cpp`. | Integrated into recovery line; still needs visual/device regression. |
| `fix/avatarforge-overexposed-render` | Lowers default exposure (1.0 to 0.65), sun intensity (3.4 to 1.5), environment intensity (1.0 to 0.65), and disables bloom by default. | Already present in `work/android-engine-dev`; no duplicate patch required. |
| `feat/json-asset-registry` | Adds bounded JSON manifest/preset loader, schemas and advanced parameter definitions. | Staged into this branch for build/integration checks. The registry is not yet wired into a user-facing asset manager, manifest installation, material/animation loaders or the settings UI. Do not describe these asset categories as runtime-supported until consumers/tests exist. |
| `feat/avatar-realism-upgrade` | Adds realism acceptance criteria and recovery notes alongside the JSON asset work. | Acceptance/recovery documents staged. It is a target contract, not proof that photorealistic geometry or textures exist. |
| `fix/renderer-recovery-2026-10-21` | Primarily old README and recovery documentation; notes an unverified external APK/AAR bundle. | Do not replace current source artifacts with that bundle. Recovery facts are preserved in the staged docs; old binaries remain unverified until checksums/build/device tests. |
| `chore/apply-dropbox-update-2026-10-21` | Large duplicated BUILD/DEVICE/STATUS documents from a supplied update; contains historic host/CI counts and explicitly says no device step was run. | Reviewed, not wholesale copied. Reconcile claims against the current exact commit before adopting historic counts as current results. |
| `main` | Diverged/stale relative to the active engine line. Its unique diff is historical documentation, not a newer verified renderer fix. | Do not merge the branch history. Selectively preserve useful docs only after reconciling them to current code. |
| `feat/repo-wide-engine-consolidation` | Behind the active branch and contains no unique ahead commits according to GitHub comparison. | No patch to apply. |
| `work/android-engine-dev` | Active engine base; current renderer work remains an open PR rather than merged. | Preserve as the integration base; do not claim it contains unmerged PR work. |

## Gates before merge

1. Run CI against the latest staged branch head, including Kotlin compilation and native CMake/Vulkan compilation.
2. Add focused tests for asset manifest parsing, path traversal rejection, dependency cycles, malformed presets, and generator integration. The current registry source alone is not proof of those tests.
3. Verify the packaged asset installation path: `AssetRegistry.open()` reads `filesDir/avatarforge/manifest.json`, but this branch does not yet install that manifest or assets automatically.
4. Run the full renderer recovery test matrix on Samsung Galaxy S24: surface create/change/destroy, failed resize recovery, Vulkan fence/submit/present failure handling, actual draw counts, visible avatar, full and stripped-morph GLB, pose/facial channels, export/reimport, and lifecycle.
5. Keep physical-device status UNVERIFIED until those checks produce real device evidence.

## Important product truth

The current generation path is a deterministic procedural avatar generator with coarse reference-image cues, not neural photorealistic identity reconstruction. The realism acceptance document specifies a goal. It does not upgrade the renderer or generator by itself.

The app remains offline-first. Do not add a network server dependency or a fake Start Server control to this branch; server controls need a real implemented service contract and health checks first.
