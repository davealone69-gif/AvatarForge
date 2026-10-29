#!/usr/bin/env python3
"""Regression tests for the narrowly pinned pre-removal Vulkan SPIR-V snapshot."""
import subprocess
import unittest
from pathlib import Path
from unittest.mock import patch

import embed_vulkan_shaders as vk


class VulkanShaderSnapshotTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.stages = sorted((p.stem, p.suffix[1:], p) for p in vk.SHADERS.iterdir()
                            if p.suffix in (".vert", ".frag", ".comp"))
        cls.header = vk.OUTPUT.read_text(encoding="utf-8")

    def test_snapshot_matches_exact_old_sources_and_header(self):
        self.assertEqual(vk.LEGACY_SOURCE_SHA256, vk.source_digest(self.stages))
        entries = vk.historical_entries(self.stages, self.header)
        self.assertIsNotNone(entries)
        self.assertEqual(13, len(entries))
        self.assertEqual(self.header, vk.embed(entries))

    def test_changed_source_is_rejected(self):
        old_source = vk.source

        def changed_source(path, stage):
            shader = old_source(path, stage)
            return shader + "// changed source\n" if path.name == "shadow.vert" else shader

        with patch.object(vk, "source", side_effect=changed_source):
            self.assertIsNone(vk.historical_entries(self.stages, self.header))

    def test_changed_embedded_word_or_comment_is_rejected(self):
        self.assertIsNone(vk.historical_entries(
            self.stages, self.header.replace("0x07230203u", "0x07230202u", 1)))
        self.assertIsNone(vk.historical_entries(
            self.stages, self.header.replace("Do not edit.", "Check manually.", 1)))

    def test_missing_compiler_is_fail_closed(self):
        with patch.dict("os.environ", {"AF_GLSLC": "/missing/glslc"}):
            with self.assertRaisesRegex(RuntimeError, "does not name a file"):
                vk.compiler()

    def test_historical_path_requires_a_real_validator(self):
        with self.assertRaisesRegex(RuntimeError, "requires spirv-val"):
            vk.validate_historical_table(self.stages, self.header, Path("/missing/spirv-val"))

    def test_historical_path_validates_all_thirteen_modules(self):
        # Mock the process boundary only; CI uses NDK's actual spirv-val on real SPIR-V.
        ok = subprocess.CompletedProcess([], 0, "", "")
        with patch.object(vk.subprocess, "run", return_value=ok) as run:
            self.assertTrue(vk.validate_historical_table(self.stages, self.header, Path("/bin/true")))
            self.assertEqual(13, run.call_count)
            self.assertTrue(all(call.args[0][1:3] == ["--target-env", "vulkan1.0"]
                                for call in run.call_args_list))


if __name__ == "__main__":
    unittest.main()
