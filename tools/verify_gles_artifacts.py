#!/usr/bin/env python3
"""Fail closed if an Android library/APK/AAB bundles or links a Vulkan loader.

Reads ELF64 little-endian DT_NEEDED entries (Android arm64/x86_64), not grep over
strings: a comment mentioning another API is not a runtime dependency.
"""
import argparse
from pathlib import Path
import struct
import zipfile


def needed_libraries(data):
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise ValueError("expected ELF64 little-endian Android library")
    endian = "<"
    phoff = struct.unpack_from(endian + "Q", data, 32)[0]
    phentsize, phnum = struct.unpack_from(endian + "HH", data, 54)
    loads, dynamic = [], None
    for i in range(phnum):
        off = phoff + i * phentsize
        kind, flags, fileoff, vaddr, _paddr, filesz, _memsz, _align = struct.unpack_from(endian + "IIQQQQQQ", data, off)
        if kind == 1:
            loads.append((vaddr, filesz, fileoff))
        elif kind == 2:
            dynamic = (fileoff, filesz)
    if dynamic is None:
        return []
    def vread(vaddr, size):
        for base, length, offset in loads:
            if base <= vaddr and vaddr + size <= base + length:
                start = offset + vaddr - base
                return data[start:start + size]
        raise ValueError("dynamic address is outside PT_LOAD")
    entries = []
    for off in range(dynamic[0], dynamic[0] + dynamic[1], 16):
        tag, value = struct.unpack_from(endian + "qQ", data, off)
        if tag == 0:
            break
        entries.append((tag, value))
    strings = next((value for tag, value in entries if tag == 5), None)
    size = next((value for tag, value in entries if tag == 10), None)
    if strings is None or size is None:
        raise ValueError("ELF dynamic section lacks string table")
    table = vread(strings, size)
    result = []
    for tag, offset in entries:
        if tag == 1:
            if offset >= len(table):
                raise ValueError("invalid DT_NEEDED string offset")
            result.append(table[offset:table.index(b"\0", offset)].decode("utf-8"))
    return result


def check_library(name, data):
    if Path(name).name == "libvulkan.so":
        raise ValueError(f"{name}: bundled Vulkan loader")
    needed = needed_libraries(data)
    if any("vulkan" in item.lower() for item in needed):
        raise ValueError(f"{name}: Vulkan DT_NEEDED dependency {needed}")
    if Path(name).name == "libafrender.so":
        if not {"libEGL.so", "libGLESv3.so"}.issubset(needed):
            raise ValueError(f"{name}: missing GLES/EGL dependencies {needed}")
    return needed


def check_file(path):
    if path.suffix == ".so":
        return [(str(path), check_library(path.name, path.read_bytes()))]
    if path.suffix not in (".apk", ".aab", ".aar", ".zip"):
        raise ValueError(f"unsupported artifact: {path}")
    found = []
    with zipfile.ZipFile(path) as archive:
        for name in archive.namelist():
            if name.endswith(".so") and ("/lib/" in "/" + name or name.startswith("jni/")):
                found.append((f"{path}:{name}", check_library(name, archive.read(name))))
    if not found:
        raise ValueError(f"{path}: no native libraries found")
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path)
    args = parser.parse_args()
    files = []
    for path in args.paths:
        if path.is_dir():
            files.extend(sorted(path.rglob("*.so")))
        else:
            files.append(path)
    if not files:
        raise ValueError("no artifacts to verify")
    count = 0
    for path in files:
        for name, needed in check_file(path):
            print(f"PASS GLES-only linkage: {name} -> {', '.join(needed)}")
            count += 1
    print(f"PASS: {count} native libraries checked; no Vulkan loader or link dependency")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, struct.error, zipfile.BadZipFile) as error:
        raise SystemExit(f"FAIL: {error}")
