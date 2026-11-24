#!/usr/bin/env python3
"""Build TD-H9 lock firmware (no APRS extras). See build_lock_lib."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_lock_lib

if __name__ == "__main__":
    raise SystemExit(build_lock_lib.main(fw_name="TD-H9-V1.0.38DKXCE.fw"))