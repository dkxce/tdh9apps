#!/usr/bin/env python3
"""Install stub/aprs_ext.s into a vendored h9toolbox module-loader tree.

    aprs_ext_ci_patch.py <module-loader-dir> <aprs_mute.s> <aprs_ext.s>

Two vendor layouts are supported:

  * build.py reads both `stub/aprs_ext.s` and `stub/aprs_mute.s` (this repo's
    build.py, shipped in the vendor archive): install both files there.
  * build.py concatenates only `src|stub/aprs_mute.s` (stock h9toolbox): append
    aprs_ext.s to that aprs_mute.s.

In both cases the script removes a previously appended aprs_ext.s block from
every other aprs_mute.s, so the assembled stub can never define
ml_beacon_tick twice, and it adds the two extra call sites to `mute_sites`
unless build.py already references them:

    0x86324 -> 0x85682   stock popup screen   -> ml_popup_beep
    0x87494 -> 0x82EB2   timed-beacon check   -> ml_beacon_tick

It fails loudly when the vendor does not look like either layout; that is a
signal to look at the vendor build.py, not to skip the check.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

MARKER = "/* ---- aprs_ext.s (installed by aprs_ext_ci_patch.py) ---- */"
DEF = re.compile(r"^[ \t]*ml_beacon_tick:", re.M)
EXTRA_SITES = ('(0x862C6, 0x55EC2, "ml_popup_beep")', '(0x87494, 0x82EB2, "ml_beacon_tick")')
# Older vendor build.py hooked the screen function (0x1E85782) that stock skips
# when the RX popup setting is on; move it to the popup request call (0x1E55FC2).
OLD_POPUP_SITE = '(0x86324, 0x85682, "ml_popup_beep")'
NEW_POPUP_SITE = '(0x862C6, 0x55EC2, "ml_popup_beep")' 


def strip_ext(text: str) -> str:
    """Drop a previously appended aprs_ext.s block, if any."""
    if MARKER not in text:
        return text
    return text.split(MARKER, 1)[0].rstrip() + "\n"


def ref(src: str, name: str):
    return re.search(rf'ROOT\s*/\s*"(src|stub)"\s*/\s*"{name}"', src)


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print(__doc__.splitlines()[2].strip(), file=sys.stderr)
        return 2
    loader = Path(argv[1]).resolve()
    mute_src = Path(argv[2]).read_text(encoding="utf-8-sig")
    ext = Path(argv[3]).read_text(encoding="utf-8-sig")
    if not DEF.search(ext):
        raise SystemExit(f"{argv[3]}: ml_beacon_tick not defined")

    build = loader / "build.py"
    src = build.read_text(encoding="utf-8")
    ext_ref = ref(src, "aprs_ext.s")
    mute_ref = ref(src, "aprs_mute.s")
    if not ext_ref and not mute_ref:
        raise SystemExit(f"{build}: neither aprs_mute.s nor aprs_ext.s is read")

    # One carrier only: clean every aprs_mute.s left over from earlier runs.
    for path in sorted(loader.glob("*/aprs_mute.s")):
        text = path.read_text(encoding="utf-8-sig")
        if MARKER in text:
            path.write_text(strip_ext(text), encoding="utf-8")
            print(f"{path}: removed a stale aprs_ext.s block")

    if mute_ref:
        target = loader / mute_ref.group(1) / "aprs_mute.s"
        target.parent.mkdir(parents=True, exist_ok=True)
        body = strip_ext(mute_src).rstrip()
        if ext_ref:
            target.write_text(body + "\n", encoding="utf-8")
            print(f"{target}: installed aprs_mute.s")
        else:
            target.write_text(body + "\n" + MARKER + "\n" + ext.rstrip() + "\n", encoding="utf-8")
            print(f"{target}: installed aprs_mute.s + aprs_ext.s")

    if ext_ref:
        target = loader / ext_ref.group(1) / "aprs_ext.s"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(ext.rstrip() + "\n", encoding="utf-8")
        print(f"{target}: installed aprs_ext.s")

    # Exactly one definition across every file the stub is built from.
    carriers = []
    for path in list(loader.glob("*/aprs_mute.s")) + list(loader.glob("*/aprs_ext.s")) + list(loader.glob("*/stub.s")):
        if DEF.search(path.read_text(encoding="utf-8-sig")):
            carriers.append(str(path.relative_to(loader)))
    if len(carriers) != 1:
        raise SystemExit(f"ml_beacon_tick carriers (need exactly one): {carriers}")

    if OLD_POPUP_SITE in src:
        src = src.replace(OLD_POPUP_SITE, NEW_POPUP_SITE)
        build.write_text(src, encoding="utf-8")
        print(f"{build}: popup hook moved to 0x862C6")
    if "ml_popup_beep" in src and "ml_beacon_tick" in src:
        print(f"{build}: hook sites already present")
        return 0
    m = re.search(r"^([ \t]*)mute_sites = \((.*)\)[ \t]*$", src, re.M)
    if not m:
        raise SystemExit(f"{build}: `mute_sites = (...)` not found")
    indent, inner = m.group(1), m.group(2)
    replacement = f"{indent}mute_sites = ({inner}, {EXTRA_SITES[0]}, {EXTRA_SITES[1]})"
    build.write_text(src[:m.start()] + replacement + src[m.end():], encoding="utf-8")
    print(f"{build}: mute_sites += popup, timed beacon")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))