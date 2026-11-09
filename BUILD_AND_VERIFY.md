# GLES GLB integrity and Surface lifecycle verification

This document is the execution record for branch `fix/gles-glb-integrity-lifecycle`.
It does not claim a successful build or phone test. The verification ledger remains
`UNVERIFIED` until the required evidence is attached.

## Scope

1. Bind real GLES buffer objects to the avatar, ground and skeleton VAOs before declaring their
   vertex attributes.
2. Assert the canonical `af_vertex` stride and member offsets at compile time.
3. Validate imported GLB accessor alignment, index component types, POSITION format and primitive
   topology before data reaches the renderer.
4. Validate every engine index and submesh range against the uploaded vertex/index buffers before
   issuing draw calls.
5. Derive perspective near/far clipping planes from the model bounds and preserve column-major
   `Projection * View` multiplication.
6. Stop frame scheduling when a Surface is destroyed, serialize renderer destruction on the
   owning HandlerThread, wait for GPU idle, unbind EGL, then destroy EGL surface/context.

## Build

Run from the repository root with the pinned toolchain described in `BUILD.md`:

```bash
tools/bootstrap_toolchain.sh
tools/build_host.sh test
tools/build_android.sh
tools/build_gradle.sh :app:assembleDebug -Paf.abIs=arm64-v8a -Paf.prebuiltJni=$PWD/prebuilt
```

The command sequence above is the required build path, not evidence that it has passed on this
branch. Record the complete stdout/stderr, exit codes, toolchain versions, commit SHA and APK SHA-256.
Do not reuse an APK from `main` or an earlier branch as evidence for this branch.

## GLB geometry checks

Use the actual model that previously rendered as fragments, plus a known-good small triangle model.
For each import, record the filename and SHA-256.

Acceptance:
- GLB import succeeds without parser warnings that affect geometry.
- The renderer reports a non-zero vertex count, index count, triangle count and draw-call count.
- No `GLES geometry rejected` or invalid submesh-range messages appear.
- The entire model is visible, connected and correctly occluded from front, back, top and side views.
- A model with deliberately corrupted accessor/index/submesh ranges is rejected or skipped safely,
  never rendered through an out-of-bounds GPU read.
- Repeated imports and mesh invalidations do not leave stale buffer counts or VAO bindings.

## Surface lifecycle checks

On the physical Android device:
1. Open the viewer and wait until a model is rendering.
2. Background the app, return, rotate/recreate the viewer if supported, and repeat at least 20 times.
3. Navigate away from the viewer and back at least 20 times.
4. Exercise Activity destruction/recreation and confirm the render HandlerThread exits cleanly.
5. Confirm no crash, ANR, EGL_BAD_SURFACE loop, use-after-free, or renderer call after destruction.

The render thread must log `render loop quiesced; EGL teardown starting` before
`renderer destroyed; EGL teardown complete` for every surface destruction. The synchronous
`surfaceDestroyed` callback must not return before the second message.

## Logcat

Start capture before opening the viewer:

```bash
adb logcat -c
adb logcat -v time -s AvatarForge:V
```

For a saved capture:

```bash
adb logcat -d -v time -s AvatarForge:V > artifacts/gles-lifecycle-logcat.txt
```

Expected order for each teardown:

```text
I/AvatarForge: surfaceDestroyed: render loop quiesced; EGL teardown starting
I/AvatarForge: surfaceDestroyed: renderer destroyed; EGL teardown complete
```

A release path uses the corresponding `release:` messages. A start message without its matching
completion message is a failure and must be investigated, not marked as passed.

## Device identity and evidence

Record:

```bash
adb shell getprop ro.product.model
adb shell getprop ro.build.version.sdk
adb shell getprop ro.product.cpu.abi
adb shell dumpsys SurfaceFlinger
```

Attach:
- branch and commit SHA;
- host-test and Android build logs with exit codes;
- APK path, byte size and SHA-256;
- GLB filename and SHA-256;
- screenshots or screen recording from front/back/side views;
- lifecycle logcat covering repeated surface destruction/recreation;
- crash-buffer and ANR checks.

## Status rules

- `REAL + VERIFIED`: build and the relevant runtime/device acceptance checks passed with evidence.
- `UNVERIFIED`: source changes exist, but required build/runtime/device evidence is absent.
- `PARTIAL`: only a clearly identified subset passed.
- Never infer correct rendering from successful GLB parsing or a successful APK build.
- Never mark lifecycle fixed from source inspection alone.
