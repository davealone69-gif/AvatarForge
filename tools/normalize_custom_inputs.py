#!/usr/bin/env python3
"""Normalize a user-supplied AvatarForge input list into a deterministic runtime contract.

The source list is never modified. Enum labels are converted to stable lower-snake identifiers and
kept as import aliases. Every input receives an explicit implementation status. The production UI
must expose only VERIFIED entries; PARTIAL and BLOCKED entries remain accepted documentation/import
vocabulary, never fake controls.
"""
from __future__ import annotations

import argparse
import hashlib
import html
import json
import re
from pathlib import Path
from typing import Any


def slug(text: str) -> str:
    value = html.unescape(text).strip().lower()
    value = re.sub(r"[^a-z0-9]+", "_", value).strip("_")
    return value or "none"


# These fields reach existing AvatarSpec/C-ABI members and have observable engine effects, but stay
# PARTIAL until the 0.10.1 Android build and physical-device inspection are complete.
BINDINGS: dict[str, dict[str, Any]] = {
    "body_height_cm": {
        "target": "AvatarSpec.heightM",
        "native_target": "af_spec.height_m",
        "transform": {"kind": "scale", "factor": 0.01},
        "supported_input_range": [140.0, 220.0],
    },
    "body_waist_cm": {
        "target": "AvatarSpec.waistCm",
        "native_target": "af_spec.waist_cm",
        "transform": {"kind": "identity"},
    },
    "body_muscle_definition": {
        "target": "AvatarSpec.muscle",
        "native_target": "af_spec.muscle",
        "transform": {"kind": "scale", "factor": 0.01},
    },
    "body_fat_percentage": {
        "target": "AvatarSpec.bodyFat",
        "native_target": "af_spec.body_fat",
        "transform": {"kind": "scale", "factor": 0.01},
    },
    "hair_color": {
        "target": "AvatarSpec.hairTone",
        "native_target": "af_spec.hair_tone",
        "transform": {"kind": "hex_srgb_to_linear_rgb"},
    },
    "skin_base_color": {
        "target": "AvatarSpec.skinTone",
        "native_target": "af_spec.skin_tone",
        "transform": {"kind": "hex_srgb_to_linear_rgb"},
    },
}

TEXTURE_KEYS = {
    "eye_iris_color", "eye_glow_intensity", "eye_glow_color", "skin_roughness",
    "skin_metallic_value", "cyber_panel_lines", "cyber_accent_glow", "armor_wear_tear",
    "clothing_fabric", "makeup_style", "makeup_lip_color", "tattoo_style",
}
RIG_KEYS = {"hair_physics_stiffness"}

VISIBLE_WHEN: dict[str, dict[str, list[str]]] = {
    "chest_bust_volume": {"chest_bust_size": ["custom"]},
    "butt_volume": {"butt_size": ["custom"]},
    "eye_glow_color": {"eye_glow_intensity": ["nonzero"]},
    "armor_shoulder_pads": {"armor_torso_type": ["roman_lorica_segmentata", "tactical_ballistic_vest", "futuristic_chest_plate", "demon_corset"]},
    "armor_knee_pads": {"armor_torso_type": ["roman_lorica_segmentata", "tactical_ballistic_vest", "futuristic_chest_plate", "demon_corset"]},
    "armor_webbing_straps": {"armor_torso_type": ["tactical_ballistic_vest", "futuristic_chest_plate"]},
    "armor_pouches": {"armor_torso_type": ["tactical_ballistic_vest", "futuristic_chest_plate"]},
    "armor_wear_tear": {"armor_torso_type": ["roman_lorica_segmentata", "tactical_ballistic_vest", "futuristic_chest_plate", "demon_corset"]},
    "makeup_lip_color": {"makeup_style": ["natural", "smokey_eye", "clown_harley_style", "gothic_demon", "cyberpunk_neon"]},
}


def normalize_entry(source: dict[str, Any]) -> dict[str, Any]:
    out = dict(source)
    out["group"] = html.unescape(str(out["group"]))
    out["label"] = html.unescape(str(out["label"]))
    if out.get("type") == "enum":
        original = list(out["options"])
        normalized = [slug(str(option)) for option in original]
        out["options"] = normalized
        out["option_labels"] = {key: html.unescape(str(label)) for key, label in zip(normalized, original)}
        out["import_aliases"] = {str(label): key for key, label in zip(normalized, original)}
        out["default"] = slug(str(out["default"]))
    key = str(out["key"])
    if key in BINDINGS:
        out["implementation_status"] = "PARTIAL"
        out["engine_binding"] = BINDINGS[key]
        out["blocked_reason"] = "Android 0.10.1 rebuild and physical-device validation pending"
    elif key in RIG_KEYS:
        out["implementation_status"] = "BLOCKED_RIG"
        out["blocked_reason"] = "No matching runtime simulation/rig parameter exists"
    elif key in TEXTURE_KEYS:
        out["implementation_status"] = "BLOCKED_TEXTURE"
        out["blocked_reason"] = "No matching material, texture, or shader parameter is wired end-to-end"
    else:
        out["implementation_status"] = "BLOCKED_ENGINE_GEOMETRY"
        out["blocked_reason"] = "No matching C ABI, SDF/mesh, material, and GLB-export implementation exists"
    if key in VISIBLE_WHEN:
        out["visible_when"] = VISIBLE_WHEN[key]
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()

    raw_bytes = args.source.read_bytes()
    source = json.loads(raw_bytes)
    if not isinstance(source, list):
        raise SystemExit("source root must be an array")
    normalized = [normalize_entry(entry) for entry in source]
    contract = {
        "schema_version": 1,
        "id": "avatar_customization",
        "title": "Avatar customization inputs",
        "source": str(args.source.as_posix()),
        "source_sha256": hashlib.sha256(raw_bytes).hexdigest(),
        "input_count": len(normalized),
        "offline_only": True,
        "ui_policy": {
            "expose_only_statuses": ["VERIFIED"],
            "hide_unimplemented": True,
            "no_placeholder_controls": True,
        },
        "notes": [
            "PARTIAL inputs have a real engine binding but remain hidden until Android device validation completes.",
            "BLOCKED inputs are import vocabulary only and must not appear as interactive production controls.",
            "The source file is preserved unchanged; enum aliases keep its original display strings importable.",
        ],
        "inputs": normalized,
    }
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_text(json.dumps(contract, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"normalized {len(normalized)} inputs -> {args.destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
