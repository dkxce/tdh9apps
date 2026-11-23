#!/usr/bin/env python3
"""Install stub/aprs_ext.s into a vendored h9toolbox module-loader tree.

    aprs_ext_ci_patch.py <module-loader-dir> <path-to-aprs_ext.s>

The vendored build.py already concatenates aprs_mute.s when --aprs-mute is
passed and retargets the sites in its `mute_sites` tuple. This script

  * appends aprs_ext.s to every aprs_mute.s the vendor build may read
    (src/aprs_mute.s and stub/aprs_mute.s), once, so the loader stub also
    carries the APRS popup beep and the V/M beacon;
  * adds the two extra call sites to `mute_sites`, once:
        0x86324 -> 0x85682   stock popup screen   -> ml_popup_beep
        0x87494 -> 0x82EB2   timed-beacon check   -> ml_beacon_tick
    (app offsets; the sector check and the patching loop both iterate that
    tuple, so nothing else in build.py changes).

It fails loudly when the vendor tree does not look like the pinned one; that
is a signal to look at the vendor build.py, not to skip the check.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

MARKER = "/* ---- aprs_ext.s (installed by aprs_ext_ci_patch.py) ---- */"
EXTRA_SITES = ('(0x86324, 0x85682, "ml_popup_beep")', '(0x87494, 0x82EB2, "ml_beacon_tick")')


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__.splitlines()[2].strip(), file=sys.stderr)
        return 2
    loader = Path(argv[1]).resolve()
    ext = Path(argv[2]).read_text(encoding="utf-8-sig")
    if not ext.strip():
        raise SystemExit(f"{argv[2]}: empty")

    candidates = [loader / "src" / "aprs_mute.s", loader / "stub" / "aprs_mute.s"]
    muted = [p for p in candidates if p.is_file()]
    if not muted:
        raise SystemExit(f"no aprs_mute.s under {loader}/src or /stub")
    body = "\n" + MARKER + "\n" + ext.rstrip() + "\n"
    for path in muted:
        text = path.read_text(encoding="utf-8-sig")
        if MARKER in text:
            print(f"{path}: already installed")
            continue
        path.write_text(text.rstrip() + body, encoding="utf-8")
        print(f"{path}: appended aprs_ext.s")

    build = loader / "build.py"
    src = build.read_text(encoding="utf-8")
    # The vendor must read one of the files we just patched.
    ref = re.search(r'ROOT\s*/\s*"(src|stub)"\s*/\s*"aprs_mute\.s"', src)
    if not ref:
        raise SystemExit(f"{build}: aprs_mute.s reference not found")
    if not (loader / ref.group(1) / "aprs_mute.s").is_file():
        raise SystemExit(f"{build}: reads {ref.group(1)}/aprs_mute.s, which is missing")
    print(f"{build}: reads {ref.group(1)}/aprs_mute.s")
    if "ml_beacon_tick" in src:
        print(f"{build}: hook sites already present")
        return 0
    m = re.search(r"^([ \t]*)mute_sites = \((.*)\)[ \t]*$", src, re.M)
    if not m:
        raise SystemExit(f"{build}: `mute_sites = (...)` not found")
    indent, inner = m.group(1), m.group(2)
    for site in EXTRA_SITES:
        if site.split(",")[2].strip(' "\'') in src:
            raise SystemExit(f"{build}: {site} already referenced somewhere")
    replacement = f"{indent}mute_sites = ({inner}, {EXTRA_SITES[0]}, {EXTRA_SITES[1]})"
    build.write_text(src[:m.start()] + replacement + src[m.end():], encoding="utf-8")
    print(f"{build}: mute_sites += popup, timed beacon")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))