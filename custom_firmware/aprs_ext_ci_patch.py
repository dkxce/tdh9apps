#!/usr/bin/env python3
"""Install stub/aprs_ext.s into a vendored h9toolbox module-loader tree.

    aprs_ext_ci_patch.py <module-loader-dir> <aprs_mute.s> <aprs_ext.s>

The vendored build.py concatenates one aprs_mute.s onto stub.s when
--aprs-mute is passed and retargets the sites in its `mute_sites` tuple.
This script

  * finds the aprs_mute.s the vendor build actually reads
    (`ROOT / "src"|"stub" / "aprs_mute.s"`), installs our copy there and
    appends aprs_ext.s to it, once;
  * removes a previously appended aprs_ext.s block from every other
    aprs_mute.s under the loader, so the concatenated stub can never define
    ml_beacon_tick twice;
  * adds the two extra call sites to `mute_sites`, once:
        0x86324 -> 0x85682   stock popup screen   -> ml_popup_beep
        0x87494 -> 0x82EB2   timed-beacon check   -> ml_beacon_tick
    (the sector check and the patching loop both iterate that tuple, so
    nothing else in build.py changes);
  * fails loudly unless ml_beacon_tick ends up in exactly one aprs_mute.s.

That failure is a signal to look at the vendor build.py, not to skip the check.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

MARKER = "/* ---- aprs_ext.s (installed by aprs_ext_ci_patch.py) ---- */"
EXTRA_SITES = ('(0x86324, 0x85682, "ml_popup_beep")', '(0x87494, 0x82EB2, "ml_beacon_tick")')


def strip_ext(text: str) -> str:
    """Drop a previously appended aprs_ext.s block, if any."""
    if MARKER not in text:
        return text
    return text.split(MARKER, 1)[0].rstrip() + "\n"


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print(__doc__.splitlines()[2].strip(), file=sys.stderr)
        return 2
    loader = Path(argv[1]).resolve()
    mute_src = Path(argv[2]).read_text(encoding="utf-8-sig")
    ext = Path(argv[3]).read_text(encoding="utf-8-sig")
    if not ext.strip() or "ml_beacon_tick" not in ext:
        raise SystemExit(f"{argv[3]}: not the APRS extras source")

    build = loader / "build.py"
    src = build.read_text(encoding="utf-8")
    ref = re.search(r'ROOT\s*/\s*"(src|stub)"\s*/\s*"aprs_mute\.s"', src)
    if not ref:
        raise SystemExit(f"{build}: aprs_mute.s reference not found")
    target = loader / ref.group(1) / "aprs_mute.s"
    print(f"{build}: reads {ref.group(1)}/aprs_mute.s")

    # One copy of aprs_ext.s, in the file the build reads; none anywhere else.
    installed = 0
    for path in sorted(loader.glob("*/aprs_mute.s")):
        if path.resolve() == target.resolve():
            continue
        text = strip_ext(path.read_text(encoding="utf-8-sig"))
        path.write_text(text, encoding="utf-8")
    target.parent.mkdir(parents=True, exist_ok=True)
    text = strip_ext(mute_src)
    if "ml_beacon_tick" in text:
        raise SystemExit(f"{argv[2]}: already carries aprs_ext.s")
    target.write_text(text.rstrip() + "\n" + MARKER + "\n" + ext.rstrip() + "\n", encoding="utf-8")
    installed = 1
    print(f"{target}: installed aprs_mute.s + aprs_ext.s")

    # Exactly one source may define it, or the assembler sees it twice.
    carriers = [str(p) for p in loader.glob("*/aprs_mute.s") if "ml_beacon_tick" in p.read_text(encoding="utf-8-sig")]
    if len(carriers) != 1 or installed != 1:
        raise SystemExit(f"aprs_ext.s carriers: {carriers}")
    if target.read_text(encoding="utf-8-sig").count("ml_beacon_tick") != 1:
        raise SystemExit(f"{target}: ml_beacon_tick is not defined exactly once")
    for stub in loader.glob("*/stub.s"):
        if "ml_beacon_tick" in stub.read_text(encoding="utf-8-sig"):
            raise SystemExit(f"{stub}: already defines ml_beacon_tick; the vendor moved")

    if "ml_beacon_tick" in src and "ml_popup_beep" in src:
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