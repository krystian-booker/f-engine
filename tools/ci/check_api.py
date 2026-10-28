#!/usr/bin/env python3
"""Fail when the committed API stubs/docs differ from the C++ bindings.

Usage: check_api.py <fengine_headless> [--update]
--update rewrites lualib/types/fengine.d.lua and docs/api.md in place.
"""
import filecmp
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TARGETS = {"fengine.d.lua": ROOT / "lualib" / "types" / "fengine.d.lua", "api.md": ROOT / "docs" / "api.md"}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    update = "--update" in sys.argv[2:]
    with tempfile.TemporaryDirectory() as folder:
        subprocess.run([sys.argv[1], "--write-api", folder], check=True, stdout=subprocess.DEVNULL)
        stale = []
        for name, target in TARGETS.items():
            fresh = Path(folder) / name
            if update:
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(fresh, target)
            elif not target.exists() or not filecmp.cmp(fresh, target, shallow=False):
                stale.append(str(target.relative_to(ROOT)))
    if stale:
        print("Out of date: " + ", ".join(stale))
        print("Run: python3 tools/ci/check_api.py <build>/fengine_headless --update")
        return 1
    print("API stubs updated" if update else "API stubs are current")
    return 0


if __name__ == "__main__":
    sys.exit(main())
