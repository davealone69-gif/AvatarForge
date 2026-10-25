# Security, privacy and supply-chain audit

Threat model: an offline, single-user Android app that imports user-chosen photos, generates
avatars on-device, and exports `.glb` files via SAF/FileProvider. There is no server, account,
network call or telemetry. Evidence is from source at commit `1aa73de`.

## 1. Attack surface inventory

| Surface | Implementation | Assessment |
|---|---|---|
| App permissions | none declared; no INTERNET by construction and CI fails if added | strong, verified in workflow |
| Exported components | one launcher Activity (`exported=true`, required); FileProvider `exported=false`; no services/receivers | minimal |
| Intent inputs | MAIN/LAUNCHER; the model/gltf-binary VIEW filter was removed (AF-002) since it had no handler | dead external entry closed |
| File URIs | SAF `CreateDocument` output opened `"wt"`; no `takePersistableUriPermission`; grants are per-Intent read only | least-privilege |
| FileProvider paths | only `filesDir/avatars/` and `cacheDir/exports/` exposed (`avatarforge_file_paths.xml`) | no root/external path exposure |
| Photo import | photo picker (`image/*`) — no storage/camera permission; EXIF correction via androidx | no broad media access |
| JNI/native parsing | direct ByteBuffer copies with capacity checks; handle validity checks; GLB length/index bounds | reviewed, see §3 |
| WebView / JS bridge | none present | attack class absent |
| Logs | no user content logging found; diagnostics contain device/model facts and user-exportable plain text only | acceptable |
| Backup | `allowBackup="false"` | app data not extracted via adb backup |

## 2. Secrets and supply chain

- No keystore, `keystore.properties`, token, `.env` or credential file is tracked (conditional
  loading in `app/build.gradle.kts`; CI generates an ephemeral debug keystore).
- Workflow token scope: `android-apk.yml` requests `permissions: contents: write` because the
  prebuilt-refresh step pushes to PR branches. This is broader than the packaging jobs need.
  **Recommendation (low):** split the refresh push into its own workflow with the write scope
  and leave the build workflow at default read. Not changed this session to avoid destabilising
  the green pipeline; tracked in REMEDIATION_PLAN.
- Third-party actions are pinned by major version only (see DEPENDENCY_AUDIT §5). No SHA pinning
  yet — supply-chain hardening backlog item.
- No dependency reaches beyond google()/mavenCentral/gradlePluginPortal; no HTTP dependency in
  Gradle files.

## 3. Untrusted-input handling (native)

- GLB reader (`af_glb_read.cpp`): header and chunk lengths are validated against file size
  (lines 565, 579), node child indices are range-checked (line 635), wrong-length matrices fall
  back to identity with a recorded warning, and malformed files fail via the error string
  instead of exceptions across JNI. Host harness cases [14]/[15] cover malformed input.
- JNI: every engine entry resolves through `asHandle`; null arrays/buffers throw
  IllegalArgumentException; `memcpy` sizes derive from native counts but are bounded by the
  JVM buffer's direct capacity before copying.
- Image analysis: packed float stride is asserted on both sides
  (`AF_IMAGE_ANALYSIS_FLOAT_COUNT` ↔ `ImageAnalysisPacking.FLOAT_COUNT`), so a schema mismatch
  fails loudly rather than reading OOB.
- Residual risks (need device/fuzz work, not assumptions of safety):
  1. no fuzz corpus for the GLB JSON parser beyond harness fixtures;
  2. PNG decode path (`af_png_decode.*`) not separately stress-tested in this audit;
  3. integer-overflow review of allocation `count * stride` expressions should be systematic
     (the JNI layer uses `jlong` products; kernel-internal multipliers were not all audited).

## 4. Privacy

- No identifiers, analytics, crash uploaders or ad libraries appear in either dependency list.
- Reference bitmaps live in app-private memory/state; nothing writes them to external storage.
- The diagnostics clipboard report contains app version, device model/OS, RAM/storage/thermal
  and renderer facts — no prompt text, no photos, no file paths beyond export parent directories
  shown in a Toast. Acceptable; the DIY guide already tells users what they are sharing.

## 5. Findings and status

| ID | Severity | Finding | Status |
|---|---|---|---|
| SEC-1 | low | build workflow holds `contents: write` though only the refresh step needs it | CONFIRMED, planned split |
| SEC-2 | low | actions pinned by major tag, not commit SHA | CONFIRMED, backlog |
| SEC-3 | low | no GLB/PNG fuzzing in CI | CONFIRMED; host malformed-GLB tests exist, corpus expansion planned |
| SEC-4 | info | dead VIEW intent filter advertised an unimplemented import | FIXED via AF-002 |
| — | — | No critical/high-severity security defect found in the reviewed surface | — |

No exploitation claim is made: this is static review plus the existing host tests, not a
penetration test. Device-only behaviours (intent delivery from hostile apps, SAF provider
edge cases) remain UNVERIFIED.
