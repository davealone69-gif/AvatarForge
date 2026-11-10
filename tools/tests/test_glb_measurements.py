import json
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
import glb_measurements as gm


def make_glb(document, binary=b""):
    document = dict(document)
    if binary:
        document["buffers"] = [{"byteLength": len(binary)}]
    encoded = json.dumps(document, separators=(",", ":")).encode("utf-8")
    encoded += b" " * ((4 - len(encoded) % 4) % 4)
    payload = binary + b"\0" * ((4 - len(binary) % 4) % 4)
    chunks = struct.pack("<II", len(encoded), gm.JSON_CHUNK) + encoded
    if binary:
        chunks += struct.pack("<II", len(payload), gm.BIN_CHUNK) + payload
    raw = struct.pack("<III", gm.MAGIC, 2, 12 + len(chunks)) + chunks
    handle, path = tempfile.mkstemp(suffix=".glb")
    with os.fdopen(handle, "wb") as stream:
        stream.write(raw)
    return path


def base_doc(mode=4, indexed=False, translation=None):
    positions = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0)
    views = [{"buffer": 0, "byteOffset": 0, "byteLength": len(positions)}]
    accessors = [{"bufferView": 0, "componentType": 5126, "count": 3,
                  "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]}]
    binary = positions
    primitive = {"attributes": {"POSITION": 0}, "mode": mode}
    if indexed:
        indices = struct.pack("<3H", 0, 1, 2)
        views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(indices)})
        accessors.append({"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"})
        primitive["indices"] = 1
        binary += indices
    node = {"mesh": 0}
    if translation is not None:
        node["translation"] = translation
    doc = {"asset": {"version": "2.0"}, "bufferViews": views, "accessors": accessors,
           "meshes": [{"primitives": [primitive]}], "nodes": [node],
           "scenes": [{"nodes": [0]}], "scene": 0}
    return doc, binary


class GlbMeasurementTests(unittest.TestCase):
    def run_extract(self, doc, binary):
        path = make_glb(doc, binary)
        try:
            return gm.extract(path)
        finally:
            os.unlink(path)

    def test_world_bounds_include_node_translation(self):
        doc, binary = base_doc(translation=[2, 3, 4])
        result = self.run_extract(doc, binary)
        self.assertEqual(result["geometry"]["world_bounds"]["min"], [2.0, 3.0, 4.0])
        self.assertEqual(result["geometry"]["world_bounds"]["max"], [3.0, 4.0, 4.0])

    def test_triangle_strip_topology(self):
        doc, binary = base_doc(mode=5)
        result = self.run_extract(doc, binary)
        self.assertEqual(result["scene"]["triangle_count_sum"], 1)

    def test_triangle_fan_topology(self):
        doc, binary = base_doc(mode=6)
        result = self.run_extract(doc, binary)
        self.assertEqual(result["scene"]["triangle_count_sum"], 1)

    def test_indexed_triangles(self):
        doc, binary = base_doc(indexed=True)
        result = self.run_extract(doc, binary)
        self.assertEqual(result["scene"]["triangle_count_sum"], 1)

    def test_non_triangle_mode_is_not_counted_as_triangles(self):
        doc, binary = base_doc(mode=1)
        result = self.run_extract(doc, binary)
        self.assertEqual(result["scene"]["triangle_count_sum"], 0)
        self.assertTrue(result["warnings"])

    def test_out_of_range_indices_rejected(self):
        doc, binary = base_doc(indexed=True)
        binary = binary[:-2] + struct.pack("<H", 9)
        path = make_glb(doc, binary)
        try:
            with self.assertRaises(gm.GlbError):
                gm.extract(path)
        finally:
            os.unlink(path)

    def test_bad_glb_magic_rejected(self):
        path = make_glb({"asset": {"version": "2.0"}})
        try:
            with open(path, "r+b") as stream:
                stream.write(b"BAD!")
            with self.assertRaises(gm.GlbError):
                gm.read_glb(path)
        finally:
            os.unlink(path)

    def test_metric_scale_is_only_applied_when_supplied(self):
        doc, binary = base_doc()
        result = self.run_extract(doc, binary)
        self.assertIsNone(result["geometry"]["height_m"])
        self.assertEqual(result["coordinate_system"]["units"], "unknown GLB coordinate units")


    def test_skinned_mesh_does_not_claim_node_only_world_bounds(self):
        doc, binary = base_doc()
        doc["nodes"][0]["skin"] = 0
        doc["skins"] = [{"joints": [0]}]
        result = self.run_extract(doc, binary)
        instance = result["geometry"]["mesh_instances"][0]
        self.assertTrue(instance["skinned"])
        self.assertIsNone(instance["world_bounds"])
        self.assertEqual(instance["bounds_status"], "UNAVAILABLE_WITHOUT_SKIN_DEFORMATION")
        self.assertIsNone(result["geometry"]["world_bounds"])
        self.assertIsNone(result["geometry"]["height_axis_units"])
        self.assertEqual(result["geometry"]["height_status"], "UNAVAILABLE_SKINNED_MESHES_EXCLUDED")

    def test_only_active_scene_nodes_contribute_to_bounds(self):
        doc, binary = base_doc()
        doc["nodes"].append({"mesh": 0, "translation": [100, 0, 0]})
        result = self.run_extract(doc, binary)
        self.assertEqual(result["scene"]["active_node_count"], 1)
        self.assertEqual(result["scene"]["mesh_instance_count"], 1)
        self.assertEqual(result["geometry"]["world_bounds"]["max"], [1.0, 1.0, 0.0])

    def test_position_accessor_must_be_float_vec3(self):
        doc, binary = base_doc()
        doc["accessors"][0]["type"] = "VEC2"
        path = make_glb(doc, binary)
        try:
            with self.assertRaises(gm.GlbError):
                gm.extract(path)
        finally:
            os.unlink(path)



if __name__ == "__main__":
    unittest.main()
