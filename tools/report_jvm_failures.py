#!/usr/bin/env python3
"""Emit GitHub annotations for Gradle/JVM failures when raw Actions logs are unavailable."""
from __future__ import annotations

import pathlib
import sys
import xml.etree.ElementTree as ET


def main() -> int:
    log_path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "sdk-jvm-tests.log")
    results_dir = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "sdk/build/test-results")
    if log_path.exists():
        log = log_path.read_text(errors="replace")
        interesting = [
            line for line in log.splitlines()
            if line.startswith("e: ") or "FAILED" in line or "Caused by:" in line
        ]
        for line in interesting[:30]:
            print("::error::" + line[:1000])
    if results_dir.exists():
        for path in sorted(results_dir.rglob("TEST-*.xml")):
            try:
                root = ET.parse(path).getroot()
            except Exception:
                continue
            for case in root.iter("testcase"):
                for tag in ("failure", "error"):
                    node = case.find(tag)
                    if node is not None:
                        message = (node.get("message") or "") + " " + (node.text or "")
                        name = f"{case.get('classname')}.{case.get('name')}"
                        print(f"::error file={path}::{name}: {message[:1000]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
