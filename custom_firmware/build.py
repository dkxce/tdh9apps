#!/usr/bin/env python3
"""Build H9 Toolbox Firmware with the RAM module loader (offline experiment).

Outputs (build/):
  h9_toolbox_modules_OFFLINE_EXPERIMENT.fw  main flash: H9 Toolbox Firmware
      (S2 resource service) with Brick Breaker removed, the loader stub in its
      cave, the hold-Back hook pointed at the stub, and 8 KiB of RAM
      (0x29F00-0x2BEFF) taken from the stock heap for modules
  h9_modules.bin   the module bundle to upload to the "modules" slot
      (dist/doom/upload.mjs SLOTS.modules: directory 0xCC0000)
  h9_toolbox_modules-manifest.json

Modules: 0 = menu (C, JieLi clang in Docker), 1 = Brick Breaker (the
hardware-tested game.s, unchanged, relinked for RAM).

    python3 -I build.py --stock-image <app.bin> --stock-package <TD-H9-V1.0.38.fw>
JIELI_TOOLCHAIN selects the extracted JieLi toolchain (default: the bundled
toolchain/jieli/ folder after setup.sh).
"""

from __future__ import annotations

import argparse
from hashlib import sha256
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
RESEARCH = ROOT.parent
TOOLS = RESEARCH / "brick-breaker" / "tools"
ISA = TOOLS / "pi32v2-isa"
sys.path.insert(0, str(ISA))
from pi32v2isa import asm, elf, ld  # noqa: E402

STOCK_SHA256 = "11a07bc0b7f23ba809451256004f26e31547f1c251bbbc5f02ef905c968f9cd3"
STOCK_PACKAGE_SHA256 = "c4d0beb5281f3a06d7a56083e1744b925f78ff9917c8364e006418ff0bbd0cd6"
# Base: H9 Toolbox Firmware (Brick Breaker + S2), resource-service --stage bb-s2.
H9TB_RAW = RESEARCH / "resource-service" / "build" / "h9_toolbox_firmware_NOT_FOR_FLASH.raw"
H9TB_APP_SHA256 = "1334a32c9ec3191ea0c8ca3f657331a5c5ce110a71185db1d075e7cd5d4a5ead"
H9TB_SECTORS = {0x5000, 0x62000, 0x8D000, 0xAA000, 0xAC000, 0xAE000, 0xAF000, 0xD9000}
BB_GAME_SHA256 = "19ab1b31a386634d04c964d748ceb838b773265e06ba842de0c3df8c25d0b536"

IMAGE_BASE = 0x01E00100
HOOK = 0x88906                       # my_app loop call, hooked by Brick Breaker
CAVE, TABLES = 0xA77F0, 0xA7E50      # Brick Breaker's game cave; S2 tables follow
OLD_LAUNCHER, SVC = 0xA9D9E, 0xA9E00  # Brick Breaker's launcher; S2 .svc follows
HEAP_END_SITE, HEAP_END_OLD = 0x05FA9E, bytes.fromhex("c5ff00bf0200")
LOAD, SLOT_END = 0x29F00, 0x2BF00
SLOT_DIRECTORY, SLOT_DATA, SLOT_LIMIT = 0xCC0000, 0xCC1000, 0xCE0000

BUNDLE_MAGIC, BUNDLE_VERSION = b"H9MODS01", 1
HEADER, ENTRY, NAME = 16, 32, 16
C_FLAGS = ["-target", "pi32v2", "-mcpu=r3", "-Oz", "-integrated-as", "-ffreestanding", "-fno-builtin",
           "-fno-common", "-ffunction-sections", "-Wall", "-Werror"]
# Extra compiler flags for test builds, e.g. H9_CFLAGS=-DAPRS_SPEAKER_TEST.
C_FLAGS += os.environ.get("H9_CFLAGS", "").split()


def crc16(data: bytes) -> int:
    """CRC-16/XMODEM, as the stub, the S2 service and the uploader compute it."""
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def bundle(modules: list[dict]) -> bytes:
    """H9MODS01 bundle: header, 32-byte entries, then 4-byte aligned bodies."""
    if not 1 <= len(modules) <= 255:
        raise ValueError("a bundle holds 1-255 modules")
    offset = HEADER + ENTRY * len(modules)
    entries, bodies = b"", b""
    for m in modules:
        body, name = m["body"], m["name"].encode("ascii")
        if len(name) > NAME or not body:
            raise ValueError(f"bad module {m['name']!r}")
        if len(body) + m["bss"] > SLOT_END - LOAD or not 0 <= m["entry"] < len(body):
            raise ValueError(f"module {m['name']!r} does not fit the {SLOT_END - LOAD}-byte slot")
        pad = (-offset) % 4
        bodies += b"\xff" * pad
        offset += pad
        entries += name.ljust(NAME, b"\0") + struct.pack("<IIIHH", offset, len(body), m["bss"], m["entry"],
                                                         crc16(body))
        bodies += body
        offset += len(body)
    data = BUNDLE_MAGIC + struct.pack("<HHI", BUNDLE_VERSION, len(modules), offset) + entries + bodies
    if len(data) != offset or len(data) > SLOT_LIMIT - SLOT_DATA:
        raise ValueError("bundle does not fit the modules slot")
    return data


def parse_bundle(data: bytes) -> list[dict]:
    """The checks the stub makes (ml_check, ml_load), for tests and tools."""
    if data[:8] != BUNDLE_MAGIC:
        raise ValueError("not an H9MODS01 bundle")
    version, count, length = struct.unpack_from("<HHI", data, 8)
    if version != BUNDLE_VERSION or not 1 <= count <= 255 or length != len(data):
        raise ValueError("bad bundle header")
    out = []
    for i in range(count):
        e = data[HEADER + ENTRY * i:HEADER + ENTRY * (i + 1)]
        offset, size, bss, entry, crc = struct.unpack_from("<IIIHH", e, NAME)
        body = data[offset:offset + size]
        if not (0 < size and size + bss <= SLOT_END - LOAD and entry < size and offset + size <= len(data)):
            raise ValueError(f"bad entry {i}")
        if crc16(body) != crc:
            raise ValueError(f"CRC mismatch in entry {i}")
        out.append({"name": e[:NAME].rstrip(b"\0").decode("ascii"), "offset": offset, "size": size, "bss": bss,
                    "entry": entry, "body": body})
    return out


def run(cmd: list, **kw) -> str:
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        raise RuntimeError(f"failed: {' '.join(map(str, cmd))}\n{r.stdout}{r.stderr}")
    return r.stdout


def assemble(source: Path, out: Path) -> Path:
    obj = out / (source.stem + ".o")
    obj.write_bytes(asm.assemble_source(source.read_text(encoding="utf-8"), filename=str(source)))
    return obj


def link(objs: list[Path], origin: int, entry: str, out: Path, name: str) -> elf.Program:
    script = out / f"{name}.ld"
    script.write_text('OUTPUT_FORMAT("elf32-pi32v2")\nOUTPUT_ARCH(pi32v2)\n'
                      f"ENTRY({entry})\nSECTIONS {{\n    . = 0x{origin:08X};\n"
                      "    .text : { *(.text .text.*) *(.rodata .rodata.*) }\n"
                      "    /DISCARD/ : { *(.comment) *(.note*) }\n}\n")
    path = out / f"{name}.elf"
    state, data = ld.link(ld.parse_args(["-T", str(script), "-o", str(path), *map(str, objs)]))
    if data is None or state.errors:
        raise RuntimeError(f"Pi32 linker failed for {name}: {state.errors}")
    path.write_bytes(data)
    program = elf.read(data)
    # The loader copies .text only and zeroes nothing: globals (even zeroed
    # ones in .bss) would land past the code, unloaded and unzeroed, possibly
    # inside the shared state at the top of the slot. Modules use fixed
    # addresses for state instead.
    loaded = [s for s in program.sections if s.name.startswith((".data", ".bss", ".sdata", ".sbss")) and s.length]
    if loaded:
        raise ValueError(f"{name}: data/bss sections are not supported: {[(s.name, s.length) for s in loaded]}")
    undefined = sorted({s.name for s in program.symbols if s.name and s.shndx == 0})
    if undefined:
        raise ValueError(f"{name}: undefined symbols {undefined}")
    return program


def text_of(program: elf.Program, origin: int) -> bytes:
    text = next(s for s in program.sections if s.name == ".text")
    if text.addr != origin:
        raise ValueError(f".text at 0x{text.addr:X}, expected 0x{origin:X}")
    return bytes(text.data)


def symbol(program: elf.Program, name: str) -> int:
    values = [s.value for s in program.symbols if s.name == name]
    if len(values) != 1:
        raise ValueError(f"symbol {name} not found exactly once")
    return values[0]


def compile_c(source: Path, out: Path, toolchain: Path) -> Path:
    obj = out / (source.stem + ".o")
    rel = lambda p: "/w/" + p.relative_to(RESEARCH).as_posix()
    run(["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{toolchain}:/opt/jieli:ro",
         "-v", f"{RESEARCH}:/w", "-w", "/w", "h9-jieli", "/opt/jieli/pi32v2/bin/clang", *C_FLAGS,
         "-I", rel(ROOT / "modules" / "common"), "-c", rel(source), "-o", rel(obj)])
    return obj


def build_modules(out: Path, toolchain: Path) -> list[dict]:
    common = ROOT / "modules" / "common"
    run([sys.executable, "-I", str(common / "font5x7.py")])
    lib = [assemble(common / "h9lib.s", out), compile_c(common / "gfx.c", out, toolchain)]

    def c_module(name: str, source: Path, *extra: Path) -> dict:
        objs = [compile_c(source, out, toolchain), *(compile_c(e, out, toolchain) for e in extra)]
        prog = link([*objs, *lib], LOAD, "module_main", out, "mod_" + source.stem)
        return {"name": name, "body": text_of(prog, LOAD), "bss": 0, "entry": symbol(prog, "module_main") - LOAD}

    menu = c_module("MENU", ROOT / "modules" / "menu" / "menu.c", ROOT / "modules" / "common" / "vfo_backup.c")
    about = c_module("ABOUT", ROOT / "modules" / "about" / "about.c")
    aprs = c_module("APRS MONITOR", ROOT / "modules" / "aprs_monitor" / "aprs_monitor.c")
    if len(aprs["body"]) > 0x2000 - 0x800:
        raise SystemExit("APRS MONITOR code overlaps its state in the top 2 KiB of the RAM slot")
    # APRS screens share state at 0x2BEE0 - 0x600 (aprs_core.h APRS_ADDR); each
    # screen's code must end below it. ".APRS INFO" is hidden from the menu.
    aprs_dir = ROOT / "modules" / "aprs"
    aprs_limit = 0x2BEE0 - 0x600 - LOAD
    core = aprs_dir / "aprs_core.c"
    text = aprs_dir / "aprs_text.c"
    screens = [c_module("APRS", aprs_dir / "aprs_list.c", core, aprs_dir / "aprs_stored.c"),
               c_module(".APRS INFO", aprs_dir / "aprs_info.c", core, text),
               c_module("APRS MAP", aprs_dir / "aprs_map.c", core),
               c_module("APRS MSG", aprs_dir / "aprs_msg.c", core, text),
               c_module("APRS SEND", aprs_dir / "aprs_send.c", aprs_dir / "aprs_core_tx.c", aprs_dir / "aprs_tx.c",
                        aprs_dir / "aprs_edit.c", text)]
    for m in screens:
        if len(m["body"]) > aprs_limit:
            raise SystemExit(f"{m['name']} code ({len(m['body'])} B) overlaps the shared APRS state ({aprs_limit} B max)")
    # SAT PASS reads APRS's last GPS fix from the shared state: stay below it
    # so the fix survives.
    # Both SAT PASS screens also stay below satshare.h's 96 bytes (just under APRS's state).
    sat_limit = aprs_limit - 0x60
    sat_dir = ROOT / "modules" / "satpass"
    satpass = c_module("SAT PASS", sat_dir / "satpass.c", sat_dir / "orbit.c", ROOT / "modules" / "common" / "fix_store_save.c")
    satplot = c_module(".SAT PLOT", sat_dir / "satplot.c", sat_dir / "orbit.c")
    sattrack = c_module(".SAT TRACK", sat_dir / "sattrack.c", sat_dir / "orbit.c")
    # The satellite table is data, not code: the hidden module ".SAT DATA" (satpack.py's body from the
    # bundled TLE snapshot; the App Manager replaces it with fresh elements and the chosen satellites).
    sys.path.insert(0, str(ROOT))   # python -I does not add the script folder
    import satpack
    sat_body, sat_info = satpack.pack(satpack.parse_tles((sat_dir / "default.tle").read_text()), satpack.load_satellites())
    (out / "satdata.bin").write_bytes(sat_body)
    satdata = {"name": ".SAT DATA", "body": sat_body, "bss": 0, "entry": 0}
    satauto = c_module(".SAT TRACKER", sat_dir / "satauto.c", sat_dir / "orbit.c", ROOT / "modules" / "common" / "vfo_backup.c")
    for m in (satpass, satplot, sattrack, satauto):
        if len(m["body"]) > sat_limit:
            raise SystemExit(f"{m['name']} code ({len(m['body'])} B) overlaps the shared SAT PASS / APRS state ({sat_limit} B max)")
    # GPS information app (reads the saved fix; stays below APRS's state too).
    gps = c_module("GPS", ROOT / "modules" / "gps" / "gps.c", ROOT / "modules" / "common" / "fix_store.c")
    if len(gps["body"]) > aprs_limit:
        raise SystemExit(f"GPS code ({len(gps['body'])} B) overlaps the shared APRS state ({aprs_limit} B max)")
    # Research probe (read-only): stock's tuning state; stays below APRS's state.
    vfo_probe = c_module("VFO PROBE", ROOT / "modules" / "vfo_probe" / "vfo_probe.c")
    if len(vfo_probe["body"]) > aprs_limit:
        raise SystemExit("VFO PROBE overlaps the shared APRS state")

    # Brick Breaker: prove game.s is the hardware-tested source (its bytes at
    # the original cave), then relink it unchanged for RAM. Its entry re-runs
    # the panel set-up, as the original launcher did.
    game = assemble(RESEARCH / "brick-breaker" / "src" / "game.s", out)
    original = text_of(link([game], IMAGE_BASE + CAVE, "hello_display_hook", out, "bb_cave_check"),
                       IMAGE_BASE + CAVE)
    if sha256(original).hexdigest() != BB_GAME_SHA256:
        raise ValueError("game.s differs from the hardware-tested Brick Breaker game")
    bb_prog = link([game], LOAD, "brick_breaker_after_stock_setup", out, "mod_brick_breaker")
    bb = {"name": "BRICK BREAKER", "body": text_of(bb_prog, LOAD), "bss": 0,
          "entry": symbol(bb_prog, "brick_breaker_after_stock_setup") - LOAD}
    # Hardware test for the stub's ml_reset (not shipped in the catalog): a
    # one-module bundle whose "menu" returns 0 at once.
    reset_test = c_module("MENU", ROOT / "modules" / "reset_test" / "reset_test.c")
    test_bundle = bundle([reset_test])
    parse_bundle(test_bundle)
    (out / "test-ml-reset-bundle.bin").write_bytes(test_bundle)
    return [menu, bb, *screens, aprs, satpass, satplot, sattrack, satauto, satdata, gps, vfo_probe, about]


def call_bytes(target: int, site: int) -> bytes:
    code = bytes(next(s for s in elf.read(asm.assemble_source(f"call {target - site - 4}\n")).sections
                      if s.name == ".text").data)
    if len(code) != 4:
        raise ValueError("hook call is not 4 bytes")
    return code


def build_firmware(stock: bytes, stock_package: Path, out: Path, aprs_mute: bool = False) -> dict:
    if not H9TB_RAW.is_file():
        raise SystemExit(f"Build H9 Toolbox Firmware first: resource-service/build.py --stage bb-s2 ({H9TB_RAW})")
    base = H9TB_RAW.read_bytes()
    if sha256(base).hexdigest() != H9TB_APP_SHA256:
        raise ValueError("H9 Toolbox Firmware base app differs from the pinned build")
    if base[HOOK:HOOK + 4] != call_bytes(OLD_LAUNCHER, HOOK):
        raise ValueError("hook does not call Brick Breaker's launcher")
    if stock[HEAP_END_SITE:HEAP_END_SITE + 6] != HEAP_END_OLD or base[HEAP_END_SITE:HEAP_END_SITE + 6] != HEAP_END_OLD:
        raise ValueError("heap-end instruction differs from the pinned one")
    if stock[CAVE:TABLES] != b"\xff" * (TABLES - CAVE) or stock[OLD_LAUNCHER:SVC] != b"\xff" * (SVC - OLD_LAUNCHER):
        raise ValueError("Brick Breaker's caves are not erased in stock")

    stub_source = ROOT / "src" / "stub.s"
    if aprs_mute:
        # aprs_mute.s gates the CH-B speaker; aprs_ext.s adds the popup beep.
        combined = out / "stub_with_mute.s"
        combined.write_text("\n".join((stub_source.read_text(),
                                        (ROOT / "stub" / "aprs_mute.s").read_text(),
                                        (ROOT / "stub" / "aprs_ext.s").read_text())))
        stub_source = combined
    stub_prog = link([assemble(stub_source, out)], IMAGE_BASE + CAVE, "ml_hotkey", out, "stub")
    stub = text_of(stub_prog, IMAGE_BASE + CAVE)
    if stub_prog.entry != IMAGE_BASE + CAVE or len(stub) > TABLES - CAVE:
        raise ValueError(f"stub ({len(stub)} B) must start the cave and end before the S2 tables")

    patched = bytearray(base)
    patched[CAVE:TABLES] = stock[CAVE:TABLES]                 # remove the game
    patched[OLD_LAUNCHER:SVC] = stock[OLD_LAUNCHER:SVC]       # remove its launcher
    patched[CAVE:CAVE + len(stub)] = stub
    patched[HOOK:HOOK + 4] = call_bytes(CAVE, HOOK)
    patched[HEAP_END_SITE + 2:HEAP_END_SITE + 6] = LOAD.to_bytes(4, "little")

    # Optional APRS hooks (--aprs-mute), retargeted to the stub: the stock
    # squelch (0x5224C) and volume (0x52228) call sites for the speaker gate,
    # the popup request call (0x55EC2) for beep-beep, and the PF action
    # dispatcher call (0x52472) that we retarget to send a beacon on Weather.
    hook_sites = []
    if aprs_mute:
        hook_sites += [(0x7B7A0, 0x5224C, "ml_mute_squelch"), (0x706A2, 0x52228, "ml_mute_volume")]
        hook_sites += [(0x862C6, 0x55EC2, "ml_popup_beep")]
        hook_sites += [(0x7E21C, 0x52472, "ml_pf_action")]
    for site, original, name in hook_sites:
        expected_call = call_bytes(original, site)
        if stock[site:site + 4] != expected_call or base[site:site + 4] != expected_call:
            raise ValueError(f"APRS hook at {site:#x} differs from stock")
        patched[site:site + 4] = call_bytes(symbol(stub_prog, name) - IMAGE_BASE, site)

    raw = out / "h9_toolbox_modules_NOT_FOR_FLASH.raw"
    raw.write_bytes(patched)

    package = out / "h9_toolbox_modules_OFFLINE_EXPERIMENT.fw"
    if package.exists():
        package.unlink()
    run([sys.executable, str(TOOLS / "h9-firmware-research" / "firmware.py"), "repack", str(stock_package),
         "--app", str(raw), "--output", str(package)])
    sys.path.insert(0, str(RESEARCH / "h9-flasher"))
    from h9_flasher import parse_firmware  # noqa: E402
    original, target = parse_firmware(stock_package).image, parse_firmware(package).image
    changed = {o for o in range(0, len(original), 4096) if original[o:o + 4096] != target[o:o + 4096]}
    expected = H9TB_SECTORS | {(HEAP_END_SITE + 0x5100) & ~0xFFF}
    expected |= {(site + 0x5100) & ~0xFFF for site, _, _ in hook_sites}
    if changed != expected:
        raise ValueError(f"changed sectors {sorted(map(hex, changed))}, expected {sorted(map(hex, expected))}")
    return {
        "app_sha256": sha256(patched).hexdigest(),
        "package_sha256": sha256(package.read_bytes()).hexdigest(),
        "flash_sha256": sha256(target).hexdigest(),
        "changed_flash_sectors": [hex(c) for c in sorted(changed)],
        "stub_bytes": len(stub), "stub_sha256": sha256(stub).hexdigest(),
        "stub_symbols": {n: hex(symbol(stub_prog, n)) for n in ("ml_hotkey", "ml_main", "ml_api", "ml_read")},
        "heap_end": hex(LOAD),
    }


def build(stock_image: Path, stock_package: Path, toolchain: Path, out: Path, aprs_mute: bool = False) -> dict:
    stock = stock_image.read_bytes()
    if sha256(stock).hexdigest() != STOCK_SHA256:
        raise ValueError("Expected the exact official v1.0.38 main application")
    if sha256(stock_package.read_bytes()).hexdigest() != STOCK_PACKAGE_SHA256:
        raise ValueError("Expected the exact official v1.0.38 .fw package")
    out.mkdir(parents=True, exist_ok=True)
    firmware = build_firmware(stock, stock_package, out, aprs_mute)
    modules = build_modules(out, toolchain)
    data = bundle(modules)
    parse_bundle(data)
    (out / "h9_modules.bin").write_bytes(data)
    # One .h9mod per app (a one-module bundle) for the App Manager's "Add module".
    (out / "modules").mkdir(exist_ok=True)
    for m in modules[1:]:
        single = bundle([m])
        parse_bundle(single)
        (out / "modules" / (m["name"].lstrip(".").lower().replace(" ", "_") + ".h9mod")).write_bytes(single)
    manifest = {
        "kind": "h9_toolbox_module_loader_experiment",
        "base": "H9 Toolbox Firmware app 1334a32c (Brick Breaker removed; S2 service kept)",
        "firmware": firmware,
        "bundle": {"slot_directory": hex(SLOT_DIRECTORY), "data": hex(SLOT_DATA), "bytes": len(data),
                   "sha256": sha256(data).hexdigest(),
                   "modules": [{"name": m["name"], "bytes": len(m["body"]), "entry": hex(m["entry"]),
                                "crc16": hex(crc16(m["body"]))} for m in modules]},
        "hardware_observation": "Not yet flashed",
        "aprs_mute": {"enabled": aprs_mute, "hardware_tested": True},
        "aprs_ext": {"enabled": aprs_mute, "hardware_tested": True,
                     "beep_tone_id": 42, "hooks": "popup request 0x862C6 -> 0x55EC2",
                     "pf_action": "0x7E21C -> 0x52472, beacon on action 6 (Weather) instead of the weather screen; "
                                  "beacon verified on hardware, screen cancellation not tested"},
    }
    (out / "h9_toolbox_modules-manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stock-image", type=Path, required=True)
    parser.add_argument("--stock-package", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "build")
    parser.add_argument("--aprs-mute", action="store_true",
                        help="APRS extras: normal-mode CH-B speaker gate and beep-beep on the APRS popup "
                             "(concatenates stub/aprs_mute.s + stub/aprs_ext.s, retargets the "
                             "squelch/volume/popup-request call sites)")
    args = parser.parse_args()
    bundled = RESEARCH / "toolchain" / "jieli" / "jieli-linux-toolchains-20260730.1"
    toolchain = Path(os.environ.get("JIELI_TOOLCHAIN", bundled)).expanduser().resolve()
    if not (toolchain / "pi32v2").is_dir():
        raise SystemExit(f"No JieLi toolchain at {toolchain}: run sh firmware-research/toolchain/jieli/setup.sh")
    print(json.dumps(build(args.stock_image.resolve(), args.stock_package.resolve(), toolchain,
                           args.output.resolve(), args.aprs_mute), indent=1))
    print("Offline build only; not hardware tested")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

