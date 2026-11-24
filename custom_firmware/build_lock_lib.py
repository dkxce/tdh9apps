#!/usr/bin/env python3
"""Shared driver for the lock-firmware builds (replaces module-loader/build.py).

The vendor archives no longer ship a copy of the build script; this file lives
in custom_firmware/ and is invoked through the thin launchers

    build_lock_fw.py                  plain lock firmware (no APRS extras)
    build_lock_aprs_mute.py           + CH-B speaker gate (aprs_mute.s)
    build_lock_aprs_mute_beep_beacon.py  + popup beep-beep + PF->SBeacon (aprs_ext.s)

Each launcher passes the vendor tree root with --module-loader
(vendor/firmware-research/module-loader), the JieLi toolchain via
JIELI_TOOLCHAIN, and the stock firmware like the old build.py did:

    python3 custom_firmware/build_lock_fw.py \
        --module-loader <ml-dir> \
        --stock-image <app.bin> --stock-package <TD-H9-V1.0.38.fw> \
        --output <out>

Outputs (build/): h9_toolbox_modules_OFFLINE_EXPERIMENT.fw, h9_modules.bin,
h9_toolbox_modules-manifest.json.
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

# Configured by main(): path to the vendor module-loader tree.
ROOT: Path
RESEARCH: Path
TOOLS: Path
CUSTOM: Path
USE_MUTE = False
USE_EXT = False
RENAME = False
FW_NAME = "TD-H9-V1.0.38DKXCE.fw"

STOCK_SHA256 = "11a07bc0b7f23ba809451256004f26e31547f1c251bbbc5f02ef905c968f9cd3"
STOCK_PACKAGE_SHA256 = "c4d0beb5281f3a06d7a56083e1744b925f78ff9917c8364e006418ff0bbd0cd6"
# Base: H9 Toolbox Firmware (Brick Breaker + S2), resource-service --stage bb-s2.
H9TB_RAW = "resource-service/build/h9_toolbox_firmware_NOT_FOR_FLASH.raw"
H9TB_APP_SHA256 = "1334a32c9ec3191ea0c8ca3f657331a5c5ce110a71185db1d075e7cd5d4a5ead"
H9TB_SECTORS = {0x5000, 0x62000, 0x8D000, 0xAA000, 0xAC000, 0xAE000, 0xAF000, 0xD9000}

IMAGE_BASE = 0x01E00100
HOOK = 0x88906                       # my_app loop call, hooked by Brick Breaker
CAVE, TABLES = 0xA77F0, 0xA7E50      # Brick Breaker's game cave; S2 tables follow
OLD_LAUNCHER, SVC = 0xA9D9E, 0xA9E00  # Brick Breaker's launcher; S2 .svc follows
HEAP_END_SITE, HEAP_END_OLD = 0x05FA9E, bytes.fromhex("c5ff00bf0200")
SHORTKEY_NAME_SITE = 0xB1893          # shortkey action label "Weather" (CPU 0x01EB1993)
SHORTKEY_NAME_OLD, SHORTKEY_NAME_NEW = b"Weather", b"SBeacon"
LOAD, SLOT_END = 0x29F00, 0x2BF00
SLOT_DIRECTORY, SLOT_DATA, SLOT_LIMIT = 0xCC0000, 0xCC1000, 0xCE0000

BUNDLE_MAGIC, BUNDLE_VERSION = b"H9MODS01", 1
HEADER, ENTRY, NAME = 16, 32, 16
C_FLAGS = ["-target", "pi32v2", "-mcpu=r3", "-Oz", "-integrated-as", "-ffreestanding", "-fno-builtin",
           "-fno-common", "-ffunction-sections", "-Wall", "-Werror"]
# -Wno-missing-braces: the vendored APRS third-party code (smartbeacon.c)
# triggers this with the newer clang, and -Werror turns it into a hard error.
C_FLAGS += ["-Wno-missing-braces"]
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
    clang = toolchain / "pi32v2" / "bin" / "clang"
    run([str(clang), *C_FLAGS, "-I", str(ROOT / "modules" / "common"),
         "-c", str(source), "-o", str(obj)])
    return obj


def build_modules(out: Path, toolchain: Path) -> list[dict]:
    common = ROOT / "modules" / "common"
    run([sys.executable, "-I", str(common / "font5x7.py")])
    lib = [assemble(common / "h9lib.s", out), compile_c(common / "gfx.c", out, toolchain)]

    def c_module(name: str, source: Path, *extra: Path) -> dict:
        objs = [compile_c(source, out, toolchain), *(compile_c(e, out, toolchain) for e in extra)]
        prog = link([*objs, *lib], LOAD, "module_main", out, "mod_" + source.stem)
        return {"name": name, "body": text_of(prog, LOAD), "bss": 0, "entry": symbol(prog, "module_main") - LOAD}

    # Only the owner-lock menu ships; the rest of the vendor catalog is left out
    # so the radio's toolbox shows just the lock flow.
    menu = c_module("MENU", CUSTOM / "menu" / "menu.c", ROOT / "modules" / "common" / "vfo_backup.c")
    return [menu]


def call_bytes(target: int, site: int) -> bytes:
    code = bytes(next(s for s in elf.read(asm.assemble_source(f"call {target - site - 4}\n")).sections
                      if s.name == ".text").data)
    if len(code) != 4:
        raise ValueError("hook call is not 4 bytes")
    return code


def build_firmware(stock: bytes, stock_package: Path, out: Path) -> dict:
    raw_base = RESEARCH / H9TB_RAW
    if not raw_base.is_file():
        raise SystemExit(f"Build H9 Toolbox Firmware first: resource-service/build.py --stage bb-s2 ({raw_base})")
    base = raw_base.read_bytes()
    if sha256(base).hexdigest() != H9TB_APP_SHA256:
        raise ValueError("H9 Toolbox Firmware base app differs from the pinned build")
    if base[HOOK:HOOK + 4] != call_bytes(OLD_LAUNCHER, HOOK):
        raise ValueError("hook does not call Brick Breaker's launcher")
    if stock[HEAP_END_SITE:HEAP_END_SITE + 6] != HEAP_END_OLD or base[HEAP_END_SITE:HEAP_END_SITE + 6] != HEAP_END_OLD:
        raise ValueError("heap-end instruction differs from the pinned one")
    if stock[CAVE:TABLES] != b"\xff" * (TABLES - CAVE) or stock[OLD_LAUNCHER:SVC] != b"\xff" * (SVC - OLD_LAUNCHER):
        raise ValueError("Brick Breaker's caves are not erased in stock")

    # The owner-lock stub is kept complete in this repo (custom_firmware/stub/),
    # not regenerated by patching the vendored module-loader/src/stub.s, so the
    # CI no longer needs a stub.patch step.
    stub_parts = [CUSTOM / "stub" / "stub.s"]
    if USE_MUTE:
        stub_parts.append(CUSTOM / "stub" / "aprs_mute.s")
    if USE_EXT:
        stub_parts.append(CUSTOM / "stub" / "aprs_ext.s")
    stub_source = out / "stub_with_mute.s"
    stub_source.write_text("\n".join(p.read_text(encoding="utf-8-sig") for p in stub_parts))

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

    # Optional APRS hooks, retargeted to the stub: CH-B speaker gate (squelch
    # 0x5224C, volume 0x52228), popup beep-beep (request 0x55EC2) and the PF
    # action dispatch call (0x52472) that sends a beacon instead of Weather.
    hook_sites = []
    if USE_MUTE:
        hook_sites += [(0x7B7A0, 0x5224C, "ml_mute_squelch"), (0x706A2, 0x52228, "ml_mute_volume")]
    if USE_EXT:
        hook_sites += [(0x862C6, 0x55EC2, "ml_popup_beep")]
        hook_sites += [(0x7E21C, 0x52472, "ml_pf_action")]
    for site, original, name in hook_sites:
        expected_call = call_bytes(original, site)
        if stock[site:site + 4] != expected_call or base[site:site + 4] != expected_call:
            raise ValueError(f"APRS hook at {site:#x} differs from stock")
        patched[site:site + 4] = call_bytes(symbol(stub_prog, name) - IMAGE_BASE, site)

    # The shortkey list takes its label from app 0xB1893 (the second of the two
    # stock "Weather" strings; the first one is "Weather Site" and stays). The
    # replacement is 7 bytes, so the string table does not shift.
    shortkey_label = None
    if RENAME:
        if patched[SHORTKEY_NAME_SITE:SHORTKEY_NAME_SITE + 7] != SHORTKEY_NAME_OLD:
            raise ValueError(f"shortkey name at {SHORTKEY_NAME_SITE:#x} differs from stock")
        patched[SHORTKEY_NAME_SITE:SHORTKEY_NAME_SITE + 7] = SHORTKEY_NAME_NEW
        shortkey_label = f"{SHORTKEY_NAME_SITE:#x}: Weather -> {SHORTKEY_NAME_NEW.decode()}"

    raw = out / "h9_toolbox_modules_NOT_FOR_FLASH.raw"
    raw.write_bytes(patched)

    package = out / FW_NAME
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
    expected |= {(SHORTKEY_NAME_SITE + 0x5100) & ~0xFFF} if RENAME else set()
    if changed != expected:
        raise ValueError(f"changed sectors {sorted(map(hex, changed))}, expected {sorted(map(hex, expected))}")
    return {
        "app_sha256": sha256(patched).hexdigest(),
        "package_sha256": sha256(package.read_bytes()).hexdigest(),
        "flash_sha256": sha256(target).hexdigest(),
        "changed_flash_sectors": [hex(c) for c in sorted(changed)],
        "stub_bytes": len(stub), "stub_sha256": sha256(stub).hexdigest(),
        "shortkey_label": shortkey_label,
        "stub_symbols": {n: hex(symbol(stub_prog, n)) for n in ("ml_hotkey", "ml_main", "ml_api", "ml_read")},
        "heap_end": hex(LOAD),
    }


def build(stock_image: Path, stock_package: Path, toolchain: Path, out: Path) -> dict:
    stock = stock_image.read_bytes()
    if sha256(stock).hexdigest() != STOCK_SHA256:
        raise ValueError("Expected the exact official v1.0.38 main application")
    if sha256(stock_package.read_bytes()).hexdigest() != STOCK_PACKAGE_SHA256:
        raise ValueError("Expected the exact official v1.0.38 .fw package")
    out.mkdir(parents=True, exist_ok=True)
    firmware = build_firmware(stock, stock_package, out)
    modules = build_modules(out, toolchain)
    data = bundle(modules)
    parse_bundle(data)
    (out / "menu.h9mod").write_bytes(data)
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
        "aprs_mute": {"enabled": USE_MUTE, "hardware_tested": True},
        "aprs_ext": {"enabled": USE_EXT, "hardware_tested": True,
                     "beep_tone_id": 42, "hooks": "popup request 0x862C6 -> 0x55EC2, gate on Report Sats",
                     "pf_action": "0x7E21C -> 0x52472, beacon on action 6 (Weather) instead of the weather screen; "
                                  "beacon verified on hardware, screen cancellation not tested"},
    }
    (out / "h9_toolbox_modules-manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    return manifest


def main(argv: list[str] | None = None, use_mute: bool = False, use_ext: bool = False, rename: bool = False,
         fw_name: str = "TD-H9-V1.0.38DKXCE.fw") -> int:
    global ROOT, RESEARCH, TOOLS, CUSTOM, USE_MUTE, USE_EXT, RENAME, FW_NAME
    USE_MUTE, USE_EXT, RENAME, FW_NAME = use_mute, use_ext, rename, fw_name
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--stock-image", type=Path, required=True)
    parser.add_argument("--stock-package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--module-loader", type=Path, required=True,
                        help="vendor tree: firmware-research/module-loader (contains src/stub.s, modules/, ...)")
    args = parser.parse_args(argv)

    global ROOT, RESEARCH, TOOLS, CUSTOM
    ROOT = args.module_loader.resolve()
    RESEARCH = ROOT.parent
    TOOLS = RESEARCH / "brick-breaker" / "tools"
    CUSTOM = Path(__file__).resolve().parent
    ISA = TOOLS / "pi32v2-isa"
    sys.path.insert(0, str(ISA))
    from pi32v2isa import asm, elf, ld  # noqa: E402, F811
    globals()["asm"], globals()["elf"], globals()["ld"] = asm, elf, ld

    bundled = RESEARCH / "toolchain" / "jieli" / "jieli-linux-toolchains-20260730.1"
    toolchain = Path(os.environ.get("JIELI_TOOLCHAIN", bundled)).expanduser().resolve()
    if not (toolchain / "pi32v2").is_dir():
        raise SystemExit(f"No JieLi toolchain at {toolchain}: run sh firmware-research/toolchain/jieli/setup.sh")
    print(json.dumps(build(args.stock_image.resolve(), args.stock_package.resolve(), toolchain,
                           args.output.resolve()), indent=1))
    print("Offline build only; not hardware tested")
    return 0