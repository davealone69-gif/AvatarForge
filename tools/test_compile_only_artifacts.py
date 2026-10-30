#!/usr/bin/env python3
"""Guard the unsigned compile-only Android artifact policy and its CI packaging contract."""

from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
workflow = (root / ".github/workflows/android-apk.yml").read_text(encoding="utf-8")
app_gradle = (root / "app/build.gradle.kts").read_text(encoding="utf-8")

# No app release signing inputs, signing configuration, or credential lookups may return.
for forbidden in (
    "signingConfigs",
    "signingConfig",
    "keystore.properties",
    "AF_RELEASE_",
    "storePassword",
    "keyAlias",
    "keyPassword",
):
    assert forbidden not in app_gradle, f"app Gradle file reintroduces production signing input: {forbidden}"
assert "verifyCompileOnlyRelease" in app_gradle, "release variants must remain guarded as compile-only"
assert "-Paf.allowUnsignedRelease=true" in app_gradle, "compile-only release opt-in must be explicit"
assert "no production signing path" in app_gradle, "Gradle must document the no-signing policy"

# Tag-based production release execution and publication must be absent.
for forbidden in (
    "signed-release:",
    "softprops/action-gh-release",
    "AF_RELEASE_",
    "refs/tags/v",
    'tags: ["v*"]',
    "contents: write",
    "AvatarForge-release-arm64.apk",
):
    assert forbidden not in workflow, f"workflow reintroduces production release behavior: {forbidden}"
assert 'branches: [main, "arena/**", "fix/**"]' in workflow, "fix branches must receive Android CI"
assert "python3 tools/test_compile_only_artifacts.py" in workflow, "CI must run this policy regression"
assert ":app:assembleDebug :app:bundleRelease :sdk:assembleRelease" in workflow, (
    "CI must keep debug APK, app AAB, and SDK AAR build tasks"
)
assert "-Paf.allowUnsignedRelease=true" in workflow, "CI must explicitly opt into compile-only release output"
for artifact in (
    "AvatarForge-debug-arm64.apk",
    "AvatarForge-release-UNSIGNED-arm64.aab",
    "AvatarForge-sdk.aar",
):
    assert artifact in workflow, f"compile-only artifact is not staged: {artifact}"
assert "UNSIGNED_COMPILE_VERIFICATION_ONLY" in workflow, "provenance must label the AAB compile-only"
assert "unexpectedly signed" in workflow, "CI must reject a signed compile-only AAB"

verify_match = re.search(r"python3 tools/verify_gles_artifacts\.py (dist/[^\n]+)", workflow)
assert verify_match, "compile-only job must run the artifact verifier"
verified_paths = set(re.findall(r"dist/\S+", verify_match.group(1)))
expected = {
    "dist/AvatarForge-debug-arm64.apk",
    "dist/AvatarForge-release-UNSIGNED-arm64.aab",
    "dist/AvatarForge-sdk.aar",
}
assert verified_paths == expected, f"CI must verify exactly its staged compile artifacts; got {sorted(verified_paths)}"

print("PASS: app release has no production signing configuration or credential path")
print("PASS: no tag-triggered signed release or GitHub Release publication remains")
print("PASS: CI preserves debug APK, explicitly unsigned compile-only AAB, and SDK AAR")
print("PASS: provenance labels the AAB compile-only and CI rejects signed AAB output")
