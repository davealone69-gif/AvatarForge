#!/usr/bin/env python3
"""Guard signed-release artifact staging and Vulkan provenance contracts."""

from pathlib import Path
import re

workflow_path = Path(__file__).resolve().parents[1] / ".github/workflows/android-apk.yml"
workflow = workflow_path.read_text(encoding="utf-8")
assert 'branches: [main, "arena/**", "fix/**"]' in workflow, "fix branches must receive Android CI"
assert "'graphics_backend': 'NATIVE_VULKAN_DEFAULT_GLES_FALLBACK'" in workflow, "provenance must identify the current backend default"
assert "'vulkan_api_target': 'VK_API_VERSION_1_3 (requested target label 1.3.304; patch ignored)'" in workflow
assert "'vulkan_shader_target': 'vulkan1.3'" in workflow
assert "'vulkan_headers': 'Android NDK 26.1.10909125 / Vulkan-Headers 1.3.237'" in workflow

marker = "  signed-release:\n"
assert marker in workflow, "signed-release job is missing"
signed_job = workflow.split(marker, 1)[1]
staged_paths = set(re.findall(r"^\s*cp\s+\S+\s+(dist/\S+)", signed_job, flags=re.MULTILINE))
assert "dist/AvatarForge-release-arm64.apk" in staged_paths, "signed APK is not staged in the release job"
assert "dist/AvatarForge-release-arm64.aab" in staged_paths, "signed AAB is not staged in the release job"
assert re.search(r"find sdk/build/outputs .* -name '\*\.aar' -exec cp .* dist/AvatarForge-sdk\.aar", signed_job), "SDK AAR is not staged in the release job"

verify_match = re.search(r"python3 tools/verify_gles_artifacts\.py ([^\n]+)", signed_job)
assert verify_match, "signed release does not run the dual-API artifact verifier"
verified_paths = set(re.findall(r"dist/\S+", verify_match.group(1)))
expected = {
    "dist/AvatarForge-release-arm64.apk",
    "dist/AvatarForge-release-arm64.aab",
    "dist/AvatarForge-sdk.aar",
}
assert verified_paths == expected, (
    f"signed job must verify exactly its own staged artifacts; got {sorted(verified_paths)}"
)
assert "AvatarForge-debug-arm64.apk" not in verify_match.group(1), "debug artifacts belong to the compile job"
assert "AvatarForge-release-UNSIGNED-arm64.aab" not in verify_match.group(1), "unsigned compile artifacts belong to another job"

print("PASS: signed release stages and verifies only its own signed APK, AAB, and SDK AAR")
print("PASS: CI triggers include fix/** and provenance separates Vulkan API, shader and header versions")
