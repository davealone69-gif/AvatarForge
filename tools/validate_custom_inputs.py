#!/usr/bin/env python3
"""Validate AvatarForge's data-driven customization input contracts.

No third-party package is used, so this runs offline in the pinned build environment. Unimplemented
controls may exist in a contract only when they carry a BLOCKED_* status; production UI code is
required to hide them until their engine implementation and device tests are complete.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

KEY = re.compile(r"^[a-z][a-z0-9_]*$")
COLOR = re.compile(r"^#[0-9A-Fa-f]{6}([0-9A-Fa-f]{2})?$")
TYPES = {"float", "int", "bool", "enum", "color"}
STATUSES = {
    "VERIFIED",
    "UNVERIFIED",
    "PARTIAL",
    "BLOCKED_ENGINE_GEOMETRY",
    "BLOCKED_TEXTURE",
    "BLOCKED_RIG",
    "BLOCKED_DEVICE_TEST",
}
BUILTIN_DEPENDENCIES = {"outfit", "footwear", "sex", "age_years", "nude_mode"}


def fail(errors: list[str], where: str, message: str) -> None:
    errors.append(f"{where}: {message}")


def validate(path: Path) -> list[str]:
    errors: list[str] = []
    try:
        payload: Any = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:  # diagnostics, not control flow inside the engine
        return [f"{path}: invalid JSON: {exc}"]

    if isinstance(payload, list):
        inputs = payload
        metadata: dict[str, Any] = {}
    elif isinstance(payload, dict) and isinstance(payload.get("inputs"), list):
        inputs = payload["inputs"]
        metadata = payload
    else:
        return [f"{path}: root must be an input array or an object containing an 'inputs' array"]

    is_contract = bool(metadata)
    if is_contract:
        if metadata.get("input_count", len(inputs)) != len(inputs):
            fail(errors, str(path), "input_count does not match the inputs array")
        exposed = metadata.get("ui_policy", {}).get("expose_only_statuses", [])
        if any(status != "VERIFIED" for status in exposed):
            fail(errors, str(path), "production UI policy may expose only VERIFIED inputs")
        source_name = metadata.get("source")
        source_hash = metadata.get("source_sha256")
        if source_name and source_hash:
            source_path = Path(source_name)
            if source_path.exists():
                actual = hashlib.sha256(source_path.read_bytes()).hexdigest()
                if actual != source_hash:
                    fail(errors, str(path), "source_sha256 does not match the preserved source file")

    if metadata.get("adult_only"):
        minimum_age = metadata.get("minimum_age_years")
        if not isinstance(minimum_age, (int, float)) or minimum_age < 18:
            fail(errors, str(path), "adult_only contracts require minimum_age_years >= 18")
        if metadata.get("ui_policy", {}).get("reject_minor_or_teen_prompts") is not True:
            fail(errors, str(path), "adult_only contracts must reject minor/teen prompts")

    keys: set[str] = set()
    for index, entry in enumerate(inputs):
        where = f"{path}:inputs[{index}]"
        if not isinstance(entry, dict):
            fail(errors, where, "entry must be an object")
            continue
        key = entry.get("key")
        if not isinstance(key, str) or not KEY.fullmatch(key):
            fail(errors, where, "key must match ^[a-z][a-z0-9_]*$")
            continue
        if key in keys:
            fail(errors, where, f"duplicate key '{key}'")
        keys.add(key)
        for required in ("group", "label", "type", "default", "requires_rebuild"):
            if required not in entry:
                fail(errors, where, f"missing '{required}'")
        kind = entry.get("type")
        if kind not in TYPES:
            fail(errors, where, f"unsupported type {kind!r}")
            continue
        if not isinstance(entry.get("requires_rebuild"), bool):
            fail(errors, where, "requires_rebuild must be boolean")

        status = entry.get("implementation_status")
        if is_contract and status is None:
            fail(errors, where, "normalized contracts require implementation_status")
        if status is not None and status not in STATUSES:
            fail(errors, where, f"unknown implementation_status {status!r}")
        if status == "VERIFIED" and not isinstance(entry.get("engine_binding"), dict):
            fail(errors, where, "VERIFIED inputs require an engine_binding")

        default = entry.get("default")
        if kind in {"float", "int"}:
            lo, hi, step = entry.get("minimum"), entry.get("maximum"), entry.get("step")
            if not all(isinstance(v, (int, float)) for v in (lo, hi, step, default)):
                fail(errors, where, "numeric inputs need numeric minimum/maximum/step/default")
            elif not (lo <= default <= hi and lo < hi and step > 0):
                fail(errors, where, "numeric range/default/step is inconsistent")
            elif kind == "int" and any(int(v) != v for v in (lo, hi, step, default)):
                fail(errors, where, "integer inputs require integral minimum/maximum/step/default")
        elif kind == "enum":
            options = entry.get("options")
            if not isinstance(options, list) or not options or not all(isinstance(v, str) for v in options):
                fail(errors, where, "enum options must be a non-empty string array")
            elif default not in options:
                fail(errors, where, f"enum default {default!r} is not in options")
            aliases = entry.get("import_aliases", {})
            if not isinstance(aliases, dict) or any(target not in options for target in aliases.values()):
                fail(errors, where, "every import alias must target a declared enum option")
            labels = entry.get("option_labels")
            if labels is not None and (not isinstance(labels, dict) or set(labels) != set(options)):
                fail(errors, where, "option_labels must label every declared enum option exactly once")
        elif kind == "bool" and not isinstance(default, bool):
            fail(errors, where, "boolean default must be true or false")
        elif kind == "color" and (not isinstance(default, str) or not COLOR.fullmatch(default)):
            fail(errors, where, "color default must be #RRGGBB or #RRGGBBAA")

    all_dependency_keys = keys | BUILTIN_DEPENDENCIES
    for index, entry in enumerate(inputs):
        if not isinstance(entry, dict):
            continue
        visible = entry.get("visible_when", {})
        if not isinstance(visible, dict):
            fail(errors, f"{path}:inputs[{index}]", "visible_when must be an object")
            continue
        for dependency, accepted in visible.items():
            if dependency not in all_dependency_keys:
                fail(errors, f"{path}:inputs[{index}]", f"unknown dependency '{dependency}'")
            if not isinstance(accepted, list) or not accepted:
                fail(errors, f"{path}:inputs[{index}]", "visible_when values must be non-empty arrays")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="+", type=Path)
    args = parser.parse_args()
    errors: list[str] = []
    for path in args.paths:
        errors.extend(validate(path))
    if errors:
        print("custom-input validation FAILED", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print(f"custom-input validation PASSED: {len(args.paths)} file(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
