#!/usr/bin/env python3
"""Install stub/aprs_ext.s into a vendored h9toolbox module-loader tree.

    aprs_ext_ci_patch.py <module-loader-dir> <aprs_mute.s> <aprs_ext.s>

Two vendor layouts are supported:

  * build.py reads both `stub/aprs_ext.s` and `stub/aprs_mute.s` (this repo's
    build.py, shipped in the vendor archive): install both files there.
  * build.py concatenates only `src|stub/aprs_mute.s` (stock h9toolbox): append
    aprs_ext.s to that aprs_mute.s.

In both cases the script removes a previously appended aprs_ext.s block from
every other aprs_mute.s, so the assembled stub can never define ml_popup_beep
twice, and it makes build.py hook exactly two APRS sites:

    0x862C6 -> 0x55EC2   stock popup request -> ml_popup_beep
    0x7E21C -> 0x52472   PF action dispatch -> ml_pf_action (beacon on Weather)

An older build.py may still carry the retired timed-beacon hook
(0x87494 -> 0x82EB2, ml_beacon_tick); that entry is removed because
aprs_ext.s no longer defines the symbol.

It fails loudly when the vendor does not look like either layout; that is a
signal to look at the vendor build.py, not to skip the check.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

MARKER = "/* ---- aprs_ext.s (installed by aprs_ext_ci_patch.py) ---- */"
DEF = re.compile(r"^[ \t]*ml_popup_beep:", re.M)
POPUP_SITE = '(0x862C6, 0x55EC2, "ml_popup_beep")'
PF_SITE = '(0x7E21C, 0x52472, "ml_pf_action")'
OLD_POPUP_SITE = '(0x86324, 0x85682, "ml_popup_beep")'
BEACON_SITE = '(0x87494, 0x82EB2, "ml_beacon_tick")'


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
        raise SystemExit(f"{argv[3]}: ml_popup_beep not defined")

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
        raise SystemExit(f"ml_popup_beep carriers (need exactly one): {carriers}")

    # build.py: retire the timed-beacon hook, move the old popup site, add ours.
    before = src
    if f", {BEACON_SITE}" in src:
        src = src.replace(f", {BEACON_SITE}", "")
    elif f"{BEACON_SITE}, " in src:
        src = src.replace(f"{BEACON_SITE}, ", "")
    else:
        src = src.replace(BEACON_SITE, "")
    if f", {OLD_POPUP_SITE}" in src:
        src = src.replace(f", {OLD_POPUP_SITE}", f", {POPUP_SITE}")
    else:
        src = src.replace(OLD_POPUP_SITE, POPUP_SITE)
    if src != before:
        build.write_text(src, encoding="utf-8")
        print(f"{build}: hook sites updated (beacon retired, popup at 0x862C6)")
    for site, label in ((POPUP_SITE, "popup"), (PF_SITE, "pf action")):
        if site in src:
            print(f"{build}: {label} hook present")
            continue
        # Mirror the shape of the popup entry: it is already wired, so its
        # container is the one this build.py uses (mute_sites tuple, one
        # element per line, or a `hook_sites += [...]` statement).
        line = next((ln for ln in src.splitlines() if POPUP_SITE in ln), None)
        if line is None:
            raise SystemExit(f"{build}: {label} hook missing and no popup entry to copy")
        indent = line[:len(line) - len(line.lstrip())]
        if "=" in line and line.rstrip().endswith(")"):
            new = line.rstrip()[:-1] + f", {site})"
        elif line.rstrip().endswith(","):
            new = f"{line}\n{indent}{site},"
        else:
            new = f"{line}\n{indent}hook_sites += [{site}]"
        src = src.replace(line, new, 1)
        build.write_text(src, encoding="utf-8")
        print(f"{build}: {label} hook added next to the popup entry")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))