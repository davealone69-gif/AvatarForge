#!/usr/bin/env python3
"""Check that Kotlin NativeBridge return types match the JNI implementation."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
KOTLIN = ROOT / "sdk/src/main/java/com/avatarforge/sdk/internal/NativeBridge.kt"
JNI = ROOT / "native/src/android/af_jni.cpp"

KOTLIN_TO_JNI = {
    "Unit": "void",
    "Int": "jint",
    "Long": "jlong",
    "Float": "jfloat",
    "Boolean": "jboolean",
    "String": "jstring",
}


def kotlin_external_returns(source: str) -> dict[str, str]:
    methods: dict[str, str] = {}
    for match in re.finditer(r"\bexternal\s+fun\s+(\w+)\s*\(", source):
        name = match.group(1)
        depth = 1
        cursor = match.end()
        while cursor < len(source) and depth:
            if source[cursor] == "(":
                depth += 1
            elif source[cursor] == ")":
                depth -= 1
            cursor += 1
        if depth:
            raise ValueError(f"Unclosed parameter list for NativeBridge.{name}")
        suffix = source[cursor:]
        return_match = re.match(r"\s*:\s*([^\n]+)", suffix)
        return_type = return_match.group(1).strip() if return_match else "Unit"
        methods[name] = return_type
    return methods


def jni_external_returns(source: str) -> dict[str, str]:
    return {
        name: return_type
        for return_type, name in re.findall(
            r"JNIEXPORT\s+(\w+)\s+JNICALL\s+"
            r"Java_com_avatarforge_sdk_internal_NativeBridge_(\w+)\s*\(",
            source,
        )
    }


def expected_jni_type(kotlin_type: str) -> str | None:
    if kotlin_type.startswith("Array<"):
        return "jobjectArray"
    return KOTLIN_TO_JNI.get(kotlin_type)


def main() -> int:
    kotlin_methods = kotlin_external_returns(KOTLIN.read_text())
    jni_methods = jni_external_returns(JNI.read_text())
    errors: list[str] = []

    for name in sorted(kotlin_methods.keys() - jni_methods.keys()):
        errors.append(f"NativeBridge.{name}: missing JNI export")
    for name in sorted(jni_methods.keys() - kotlin_methods.keys()):
        errors.append(f"JNI {name}: no Kotlin external declaration")
    for name in sorted(kotlin_methods.keys() & jni_methods.keys()):
        expected = expected_jni_type(kotlin_methods[name])
        actual = jni_methods[name]
        if expected is None:
            errors.append(f"NativeBridge.{name}: unsupported Kotlin return type {kotlin_methods[name]!r}")
        elif expected != actual:
            errors.append(
                f"NativeBridge.{name}: Kotlin {kotlin_methods[name]} expects {expected}, JNI returns {actual}"
            )

    if errors:
        print("JNI bridge signature mismatch:", file=sys.stderr)
        print("\n".join(f"- {error}" for error in errors), file=sys.stderr)
        return 1
    print(f"JNI bridge return-type parity: {len(kotlin_methods)} methods, 0 mismatches")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
