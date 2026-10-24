# Renderer recovery update (2026-10-21)

> **2026-10-24 follow-up:** source audit found three concrete initialisation defects behind the
> black-viewer / 0 FPS report (missing EGL native-visual matching that ANGLE rejects, an
> unprobed Vulkan depth format, and a single-shot renderer create with no recovery). The fixes and
> the required native rebuild are documented in
> [RENDERER_FIX_2026-10-24.md](RENDERER_FIX_2026-10-24.md). Hardware verification is still pending.

## Purpose

This update bundle contains a rebuilt ARM64 debug APK and matching SDK AAR intended to retest the black viewer / 0 FPS rendering failure reported on a Samsung Galaxy S24 using Xclipse 940 / ANGLE.

**This is a retest candidate, not a verified fix.** The supplied report explicitly says the recovery build has not yet been retested on the physical phone. Do not describe the renderer as fixed until device evidence confirms visible geometry, non-zero frame rate, generation, and lifecycle/resize recovery.

## Files in the supplied bundle

| File | Size (bytes) | SHA-256 |
|---|---:|---|
| `avatarforge-render-recovery-debug-arm64.apk` | 10,342,002 | `5fab82892c9e27bbf7125e1d05d4e9cea9ea162b30180f686a76f2659c6f236c` |
| `avatarforge-render-recovery-sdk-arm64.aar` | 1,339,885 | `a80708e68146055823ebd7cf749fff4fd6027aef05674f8bc6daee28a4607ffa` |

The checksums above are copied from the supplied `SHA256SUMS.txt`; verify against the downloaded files before installation or SDK integration.

## Download source

The original Dropbox folder is the current source for the binary bundle:
[AvatarForge renderer recovery bundle on Dropbox](https://www.dropbox.com/scl/fo/s22v1herlm6dgzc825gx7/AOAZnyUA6jJ3KC1NXT_pXWs?rlkey=b7jvxccotfdaqoab72c88x9qv&st=bml9bve2&dl=0)

The binaries have **not** been uploaded as GitHub release assets by this documentation change. Do not treat this page as a direct APK download.

## Retest checklist

1. Verify APK SHA-256 against the manifest.
2. Install the APK on the affected Samsung device.
3. Open the viewer and confirm actual rendered triangles/visible avatar geometry and a live frame counter.
4. Exercise initial surface creation, rotation/resize, background/foreground, and the one-shot renderer restart.
5. Run an actual avatar generation and inspect the result; do not count a successful button press or progress completion as generation success.
6. Confirm offline behavior with airplane mode enabled.
7. Capture device diagnostics and logs. Record pass/fail for each step in `DEVICE.md` or the corresponding test report.

## Honest status

- Bundle contents and manifest: **PRESENT** in the supplied ZIP.
- SHA-256 values: **DOCUMENTED**, must be verified against the downloaded files.
- Android build: **REPORTED PASS** by the supplied update notes; not independently rebuilt during this documentation integration.
- Physical-device renderer recovery: **UNVERIFIED**.
- Production release signing: **BLOCKED** unless a valid release keystore is configured.
- GitHub release binary assets: **MISSING** as of this change.
