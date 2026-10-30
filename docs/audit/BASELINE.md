# AvatarForge engineering baseline

Inspected **2026-10-29** before the remediation described in this directory. This is a baseline, not a completion claim.

## Repository and environment

| Item | Observed value |
|---|---|
| Working directory | `/home/user/AvatarForge` |
| Repository | https://github.com/davealone69-gif/AvatarForge (public) |
| Working branch / initial HEAD | `arena/aaf85469-avatarforge` / `9f02d15680d85bb1773afe059fd048e8fe243bdd` |
| Initial working tree | Clean; local HEAD equalled its remote tip |
| Default branch | `main`, `40ef65ba74686a119d8a576d740a7981fad6f41c` |
| Active PR | #20, open draft, mergeable at the baseline; no merge/release authorized or performed |
| History limitation | Shallow clone, initially grafted at `40ef65b`; eight session commits above main. Remote tips were fetched read-only for comparison, without switching branches. |
| Releases / tags | GitHub release inventory empty; no local tags |
| Modules | `:app` and `:sdk`; `engine/` and `rust/` are not Gradle modules |
| App identity | `com.avatarforge`, versionCode 2, versionName 0.10.1 |
| Android SDK | compile/target 34, min 26; NDK 26.1.10909125; CMake 3.22.1 |
| Managed toolchain | AGP 8.5.2, Kotlin 1.9.24, Compose compiler 1.5.14, Java target 17, Gradle wrapper 8.9 |
| Native | C++17 kernel, renderer and JNI; dependency-free Rust workspace declares version 0.9.0 / minimum Rust 1.70 |
| Host | Linux x86_64, GCC/G++ and Python 3.11.2; approximately 3.8 GiB RAM, 20 GiB free storage |
| Missing locally | JDK/Javac, Gradle distribution, Android SDK/NDK, adb, emulator, CMake, Ninja, Clang, Rust/Cargo, glslc |
| Device | No connected Android device or emulator; no local GPU runtime verification possible |

The Gradle wrapper JAR SHA-256 at baseline was `498495120a03b9a6ab5d155f5de3c8f0d986a449153702fb80fc80e134484f17`. A hash alone does not establish publisher authenticity; the release/dependency audit records the remaining verification needed.

## Commands and evidence collected

Read-only inspection included:

```sh
pwd; git rev-parse --show-toplevel; git status --short --branch; git rev-parse HEAD
git remote -v; git branch -a; git tag --list; git log --oneline --decorate -15
git rev-parse --is-shallow-repository; git count-objects -vH; git reflog -8 --date=iso
git ls-remote --heads origin
gh pr view 20 --json number,url,state,isDraft,headRefName,baseRefName,headRefOid,title,mergeable
gh pr list --state all --limit 40 --json number,title,state,isDraft,headRefName,baseRefName,headRefOid,url
gh api 'repos/davealone69-gif/AvatarForge/branches?per_page=100'
gh api repos/davealone69-gif/AvatarForge/releases
gh run list --branch arena/aaf85469-avatarforge --limit 6 --json databaseId,headSha,status,conclusion,event,createdAt,url
sha256sum gradlew gradlew.bat gradle/wrapper/gradle-wrapper.jar gradle/wrapper/gradle-wrapper.properties
java -version; rustc --version; cargo --version; cmake --version; ninja --version; clang --version; adb devices -l
uname -a; free -h; df -h . /home/user
```

The missing-tool commands failed with `command not found`; they are environment limitations, not successful checks. Repository configuration, manifests, workflows, source inventories, existing status/build/device documents, UI handlers and native entry points were read. Deeper findings are recorded separately as inspection proceeds.

GitHub compare API results were collected for all **19 non-main remote branch tips**. Eight relevant tips were fetched at depth one for direct source comparison: PR #19, the integration branch, the Android development branch, Vulkan/frame-loop/telemetry fixes and quality gates. No remote branch was reset, merged, renamed or deleted. Full comparison JSON is transient evidence under `/home/user/audit-evidence/`; durable conclusions belong in these audit documents.

## Verified state at baseline

- **REAL + VERIFIED — source/CI association:** GitHub Actions run [40848746672](https://github.com/davealone69-gif/AvatarForge/actions/runs/40848746672) succeeded for exact commit `9f02d15680d85bb1773afe059fd048e8fe243bdd`. Its job built ARM64 native libraries from source, ran SDK JVM tests and renderer host tests, built debug/release APKs and an SDK AAR, audited permissions/ABI and uploaded an artifact. This does not verify installed-device behavior.
- **REAL + VERIFIED — supplied device evidence, scoped:** earlier user-supplied SM-S921B reports/screenshots show visible GLES-rendered avatars, reported 90.7 MiB atlas texture accounting and backend-fallback journal records. They do not verify the newest camera fix, sustained stability, Vulkan rendering, animation correctness or export/reimport fidelity. Original evidence remains outside Git under `/home/user/fault-review/current-build-2026-10-29/`.
- **UNVERIFIED — latest camera repair:** live-state gesture handling and explicit camera controls compile in CI, but no post-install drag/pinch/slider evidence exists.
- **PARTIAL — release delivery:** CI produces APKs/AAR, not an AAB. The release variant silently uses a debug key when production credentials are absent. Existing filenames can be mistaken for production releases. Production signing is **BLOCKED** by missing credentials.
- **UNVERIFIED — full runtime/performance:** no device is attached here. A screenshot or frame-submission counter is not a GPU benchmark or a long-duration test.
- **MISSING — external AI services:** source/manifest inventory found an offline deterministic generator, not an AI model runtime, HTTP provider, database, WebView or local service bridge. The earlier user-selected stability scope remains **no AI**; this audit does not silently add one.

## Immediate baseline risks

1. Existing README/STATUS statements conflict with newer device evidence and with the current test count; they must not remain competing current-state authorities.
2. Release signing/provenance and AAB delivery need an explicit fail-closed production path.
3. Renderer telemetry, frame-loop lifecycle, native handle ownership and untrusted GLB/PNG limits require end-to-end inspection before claiming correctness.
4. Several remote branches contain unique renderer, geometry, diagnostics and build work. They require selective comparison, not a blind merge.
5. The full kernel harness previously reported **406 checks / one failure** (embedded shader stage count 13 versus expected 12); the focused renderer suite reported **44 / zero**. Neither result may be relabelled as a fully green kernel suite.

## Preservation and next action

No user changes were discarded. No protected branch, release or signing secret was altered. The next action is a severity-ranked trace of actual execution paths, followed by small tested fixes. [PROJECT_STATUS.md](../../PROJECT_STATUS.md) is the current-state entry point; the findings and verification records distinguish source proof, CI proof and device proof.


## Current working-tree continuation — 2026-10-30

This addendum records the separate environment and Git snapshot used for the Vulkan-restoration pass; it does not rewrite the historical `/home/user/AvatarForge` baseline above.

| Item | Current observation |
|---|---|
| Working directory | `/workspace/AvatarForge` |
| Branch / initial task HEAD | `fix/verify-clean-checkout-output-dir` / `187fa25` |
| Task-start working tree | Clean; the three pre-existing local commits were preserved before this pass made changes |
| Remote base | `origin/arena/c87f5c50-avatarforge`, refreshed to `f82287b`; common ancestor with this fix branch `1dfcf40` |
| Divergence at last read-only check | Local branch was 3 commits ahead / 2 behind the refreshed arena base; no merge, rebase, reset or force-push was performed |
| New branch target | Same-name `origin/fix/verify-clean-checkout-output-dir` was absent at the last read-only check; confirm again immediately before pushing |
| Android toolchain | Android SDK `/home/ubuntu/Android/Sdk`; NDK `26.1.10909125`; NDK `glslc` and `spirv-val` available |
| Other toolchains | Linux x86_64, GCC/G++, Python 3, JDK/Gradle and Rust/Cargo available for this pass |
| Device | `adb devices -l` returned no attached phone or emulator; physical-device verification remains unavailable |

The refreshed remote tip and divergence are intentionally recorded separately from the original checkout. The authorized publication target is a new fix branch only; the arena base branch is not a push target.
