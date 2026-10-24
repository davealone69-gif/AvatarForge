# Dependency audit

Evidence: `app/build.gradle.kts`, `sdk/build.gradle.kts`, root `build.gradle.kts`,
`gradle.properties`, `rust/Cargo.toml` at commit `1aa73de`. No dependency scanning service is
reachable from the audit sandbox; CVE claims are therefore limited to version-age facts, not
asserted vulnerabilities.

## 1. Toolchain pinning

| Component | Pinned version | Notes |
|---|---|---|
| Gradle wrapper | 8.9 | wrapper properties only, checked into git |
| Android Gradle Plugin | 8.5.2 | root plugins block |
| Kotlin Android plugin | 1.9.24 | Compose compiler ext 1.5.14 matched to it |
| Compose BOM | 2024.06.00 | single BOM for all `androidx.compose.*` |
| NDK / CMake | 26.1.10909125 / 3.22.1 | CI installs exact revisions |
| compile/target SDK | 34 | min SDK 26 (Android 8.0) |
| Java/Kotlin target | 17 | both modules |

## 2. Runtime dependencies (shipped in APK)

### `:app`
| Coordinate | Version | Purpose | Assessment |
|---|---|---|---|
| `androidx.compose.ui:ui` (+graphics, tooling-preview) | BOM-managed | UI toolkit | appropriate; tooling is debug-only |
| `androidx.compose.material3:material3` | BOM-managed | components | appropriate |
| `androidx.activity:activity-compose` | 1.9.0 | ComponentActivity integration | appropriate |
| `androidx.core:core-ktx` | 1.13.1 | platform helpers | duplicated in :sdk (harmless, same version) |
| `androidx.exifinterface:exifinterface` | 1.3.7 | photo orientation before pixel upload | justified in comment; real use verified |
| `androidx.lifecycle:lifecycle-runtime-compose` | 2.8.2 | `collectAsStateWithLifecycle`-class use | appropriate |
| `org.jetbrains.kotlinx:kotlinx-coroutines-android` | 1.8.1 | job/await/cancellation | also in :sdk, same version |

### `:sdk`
| Coordinate | Version | Purpose | Assessment |
|---|---|---|---|
| `androidx.core:core-ktx` | 1.13.1 | context/services | minimal |
| `androidx.exifinterface:exifinterface` | 1.3.7 | image uprighting | shared concern with app |
| `kotlinx-coroutines-android` | 1.8.1 | job machinery | appropriate |
| JUnit 4.13.2 (test only) | — | 7 pure-JVM tests | no Robolectric; runs on plain JVM |

### Native
- System libraries only: `libvulkan`, `libEGL`, `libGLESv3`, `libandroid`, `liblog`, static
  libc++ and zlib. No third-party native runtime, no game engine, no networking stack — matches
  the offline/no-INTERNET guarantee.
- Shaders are pre-baked headers (`af_shaders_vk.h`, `af_shaders_gl.h`); no runtime shader
  compiler dependency.

## 3. Rust workspace (`rust/`)

- Present: `af-core` (engine reimplementation), `af-sys` (FFI bind), `af` (binary). Not built by
  Gradle/CI (AF-006). Cargo.lock is committed (good for reproducibility) but its dependency
  tree is currently irrelevant to shipped artifacts. Any future CI use must run
  `cargo deny`/`cargo audit` in that job.

## 4. Build hygiene facts

- No version catalog (`gradle/libs.versions.toml`): coordinates are hardcoded; the Compose BOM
  prevents Compose drift, but the six non-BOM coordinates must be bumped by hand (AF-013).
- `enableJetifier=false`, non-transitive R class, no daemon/incremental in the verified CI
  profile — deliberate low-RAM choices (`gradle.properties` comments).
- R8 disabled with a documented JNI-name rationale and complete keep rules in
  `sdk/consumer-rules.pro`; no hidden reflection beyond the JNI bridge.
- No signing secrets, `local.properties`, or keystores are tracked (verified by file inventory;
  keystore loading is conditional on an untracked `keystore.properties`).
- Dependabot/renovate configuration: absent. Given the small pinned set, a Dependabot config for
  GitHub Actions + Gradle is the lowest-effort supply-chain improvement (roadmap, not urgent).

## 5. Third-party GitHub Actions in CI

| Action | Version | Risk note |
|---|---|---|
| `actions/checkout` | v4 | pinned major; Node 20 deprecation warning observed in CI annotations |
| `actions/setup-java` | v4 | same; cache: gradle enabled |
| `android-actions/setup-android` | v3 | community action; consider Google-maintained alternatives long-term |
| `actions/upload-artifact` | v4 | first-party |
| `softprops/action-gh-release` | v2 | only runs on `v*` tags, which do not exist yet |

## 6. Verdict

Dependency surface is small, pinned, and explainable. No unused runtime dependency was found
(each maps to a verified call site). Open items are process-level: version catalog (AF-013),
advisory automation, and keeping action majors current. No dependency is added or removed in
this audit.
