#!/usr/bin/env python3
"""Host regressions for the dual-native-renderer ELF/package linkage gate."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import verify_gles_artifacts as verifier


class RendererArtifactTests(unittest.TestCase):
    def test_renderer_requires_all_three_system_graphics_libraries(self):
        needed = ["libafcore.so", "libEGL.so", "libGLESv3.so", "libvulkan.so"]
        with patch.object(verifier, "needed_libraries", return_value=needed):
            self.assertEqual(verifier.check_library("libafrender.so", b"ELF"), needed)
        with patch.object(verifier, "needed_libraries", return_value=needed[:-1]):
            with self.assertRaisesRegex(ValueError, "missing.*Vulkan"):
                verifier.check_library("libafrender.so", b"ELF")

    def test_only_renderer_links_system_loader(self):
        with patch.object(verifier, "needed_libraries", return_value=["libvulkan.so"]):
            with self.assertRaisesRegex(ValueError, "only libafrender"):
                verifier.check_library("libavatarforge.so", b"ELF")
        with patch.object(verifier, "needed_libraries", return_value=["libvulkan_custom.so"]):
            with self.assertRaisesRegex(ValueError, "unexpected Vulkan"):
                verifier.check_library("libafrender.so", b"ELF")

    def test_bundled_loader_is_rejected_even_inside_apk(self):
        with self.assertRaisesRegex(ValueError, "bundled Vulkan loader"):
            verifier.check_library("libvulkan.so", b"bad")
        with tempfile.TemporaryDirectory() as temp:
            apk = Path(temp) / "test.apk"
            with zipfile.ZipFile(apk, "w") as archive:
                archive.writestr("lib/arm64-v8a/libvulkan.so", b"bad")
            with self.assertRaisesRegex(ValueError, "bundled Vulkan loader"):
                verifier.check_file(apk)


if __name__ == "__main__":
    unittest.main()
