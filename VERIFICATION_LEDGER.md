# Verification ledger: GLES GLB integrity and Surface lifecycle

Branch: `fix/gles-glb-integrity-lifecycle`  
Base: `main`  
Device target: Samsung SM-S921B, arm64-v8a  
Last updated: 2026-11-10

| ID | Task | Source change | Build evidence | Device evidence | Status |
|---|---|---|---|---|---|
| GEO-01 | GLES VAO buffer binding and canonical vertex layout | VAO buffer-name allocation moved before attribute setup; `af_vertex` stride/member offsets guarded by static assertions | Not run on this branch | Not run | **UNVERIFIED** |
| GEO-02 | GLB accessor and index integrity | Accessor alignment, POSITION format, index component type and triangle primitive mode validation added | Not run on this branch | Not run | **UNVERIFIED** |
| GEO-03 | Engine index and submesh bounds | CPU index validation and submesh index/vertex range validation added before GPU upload/draw | Not run on this branch | Not run | **UNVERIFIED** |
| GEO-04 | MVP and clipping | Column-major `Projection * View` retained; near/far planes derived from model bounds | Not run on this branch | Not run | **UNVERIFIED** |
| LIFE-01 | Surface destruction synchronisation | Stop frame scheduling, serialize teardown on HandlerThread, wait without timeout | Not run on this branch | Not run | **UNVERIFIED** |
| LIFE-02 | GPU idle and EGL teardown order | Wait for GPU idle; unbind EGL context before destroying surface/context | Not run on this branch | Not run | **UNVERIFIED** |
| BUILD-01 | Host tests and Android debug APK | Required commands documented in `BUILD_AND_VERIFY.md` | Not run on this branch | N/A | **UNVERIFIED** |
| DEVICE-01 | Real GLB rendering on SM-S921B | Requires installation of this branch's APK | Pending | Pending | **UNVERIFIED** |
| DEVICE-02 | Repeated Surface lifecycle on SM-S921B | Logcat evidence required | Pending | Pending | **UNVERIFIED** |

## Evidence required to change status

- GEO-01 through GEO-04: successful host tests and Android native build, plus on-device rendering of
  the previously fragmented GLB from multiple viewpoints.
- LIFE-01 and LIFE-02: successful Android build and repeated physical-device Surface destruction /
  recreation with ordered logcat messages and no crash/ANR.
- BUILD-01: attach fresh branch build logs and APK SHA-256.
- DEVICE-01 and DEVICE-02: user-confirmed SM-S921B test results and raw evidence.

No task in this ledger is marked fixed or verified by source inspection alone.
