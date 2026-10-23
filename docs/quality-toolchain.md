# Repository quality toolchain

This repository uses layered checks. No single scanner or AI review is treated as proof that a build or renderer is correct.

## Automated GitHub checks

- `.github/workflows/android-quality.yml`: runs unit tests, Android lint, and a debug APK build; uploads the APK as a workflow artifact.
- `.github/workflows/semgrep.yml`: scans on pull requests, pushes to main, manual dispatch, and weekly schedule.
- `.coderabbit.yaml`: review guidance for Vulkan/native C++, Kotlin/JNI, and Gradle changes.

These checks are proposed on a draft PR. They are not active on main until the PR is reviewed and merged. First workflow results must be checked for project-specific issues.

## Local review with Ollama

The phone's Ollama endpoint is local to that device. GitHub-hosted runners cannot reach `127.0.0.1:11434` on your phone. Keep private/local AI review on the device or on a trusted self-hosted runner; do not expose Ollama to the public internet.

Suggested local preflight in Termux:

```sh
ollama list
curl -fsS http://127.0.0.1:11434/api/tags
```

Before using GHAGGA or another local review CLI, verify the repository URL, license, maintained release, install instructions, and configured provider. Run it first against a disposable branch with known seeded defects. Confirm it actually uses Ollama rather than silently falling back to a cloud provider.

## Review sequence

1. Read the diff and identify the intended behavior.
2. Run `./gradlew --no-daemon testDebugUnitTest lintDebug assembleDebug`.
3. Review Semgrep findings; fix genuine issues and document justified suppressions.
4. Review AI comments independently. AI output is advisory, not a test result.
5. Inspect native compiler output and test Vulkan failure paths, including acquire/present errors, swapchain recreation, surface/device loss, synchronization, and object lifetime.
6. Install the APK on a physical Android device and exercise the relevant flows. CI cannot claim this step was performed.
7. Only call the change verified when the build and relevant physical-device runtime checks actually pass.

## Status vocabulary

- **REAL + VERIFIED**: observed working through relevant build and runtime verification.
- **UNVERIFIED**: implementation exists, but required execution/runtime evidence is missing.
- **PARTIAL**: some checks passed, others remain outstanding.
- **MISSING**: capability or required check does not exist.
- **BLOCKED**: a specific dependency or permission prevents the check.

## Limits

- Semgrep and Android lint are static checks, not runtime proof.
- AI reviewers can miss defects or invent issues; validate each finding.
- A debug APK artifact is not a signed production release.
- Release signing secrets must remain in GitHub Secrets or a secure local secret store, never in source control.
- A physical-device render test remains required for a renderer release.
