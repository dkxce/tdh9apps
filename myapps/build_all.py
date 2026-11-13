#!/usr/bin/env python3
"""Build every app under myapps/ into top-level myapps/*.h9mod.

Discovers the apps: every myapps/<name>/src/*.c one. Each is built to
myapps/<name>/<name>.h9mod (same folder) via build_one.py, then a copy is put
at myapps/<name>.h9mod so the catalog view/CI artifact is uniform.

Usage:
  JIELI_TOOLCHAIN=<extracted-jieli> python3 myapps/build_all.py
  # or with no toolchain on the build machine, it prints the CI hint and exits 1
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def apps():
    result = []
    for app_dir in ROOT.iterdir():
        if not app_dir.is_dir():
            continue
        if app_dir.name in ("shared", "build", "toolchain"):
            continue
        if (app_dir / "src").is_dir():
            result.append(app_dir.name)
    return result


def main():
    names = sorted(apps())
    if not names:
        print("No apps found under myapps/*/src/*.c")
        return 1
    print("Apps to build: %s" % ", ".join(names))
    rc = 0
    for name in names:
        r = subprocess.run([sys.executable, str(ROOT / "build_one.py"), name,
                            "--out", str(ROOT / (name + ".h9mod"))])
        if r.returncode:
            rc = r.returncode
            print("FAILED: %s" % name)
    return rc


if __name__ == "__main__":
    sys.exit(main())