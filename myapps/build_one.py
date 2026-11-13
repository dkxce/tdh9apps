#!/usr/bin/env python3
"""Build ONE of the apps in myapps/ into an H9MODS01 .h9mod bundle.

Ported from h9toolbox's firmware-research/module-loader/build.py but for the
apps living under myapps/<name>/ :

  myapps/<name>/src/<name>.c      -- the module's C source
  myapps/shared/common/           -- shared modules (gfx.c, h9lib.s, ...)
  myapps/shared/third_party/pi32v2-isa/ -- python128 assembler/linker

Usage:
  python3 myapps/build_one.py bootscreen --out bootscreen.h9mod
  JIELI_TOOLCHAIN=<extracted-jieli> python3 myapps/build_one.py <name> [--module-name X]

On the build machine it needs the JieLi toolchain (JIELI_TOOLCHAIN pointing at
a folder that has pi32v2/bin/clang). The CI workflow downloads it.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent          # myapps/
SHARED = ROOT / "shared"
ISA = SHARED / "third_party" / "pi32v2-isa"
sys.path.insert(0, str(ISA))
from pi32v2isa import asm, elf, ld  # noqa: E402

LOAD = 0x29F00
SLOT_END = 0x2BF00
BUNDLE_MAGIC = b"H9MODS01"
BUNDLE_VERSION = 1
HEADER, ENTRY, NAME = 16, 32, 16
C_FLAGS = ["-target", "pi32v2", "-mcpu=r3", "-Oz", "-integrated-as", "-ffreestanding",
           "-fno-builtin", "-fno-common", "-ffunction-sections", "-Wall", "-Werror"]
MAX_SIZE = SLOT_END - LOAD  # 0x2000, 8 KiB


def crc16(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def bundle(modules):
    offset = HEADER + ENTRY * len(modules)
    entries, bodies = b"", b""
    for m in modules:
        body, name = m["body"], m["name"].encode("ascii")
        if len(body) + m["bss"] > MAX_SIZE or not (0 <= m["entry"] < len(body)):
            raise RuntimeError("module %s does not fit the slot" % name)
        pad = (-offset) % 4
        bodies += b"\xff" * pad
        offset += pad
        entries += name.ljust(NAME, b"\0") + struct.pack("<IIIHH", offset, len(body),
                                                        m["bss"], m["entry"], crc16(body))
        bodies += body
        offset += len(body)
    data = BUNDLE_MAGIC + struct.pack("<HHI", BUNDLE_VERSION, len(modules), offset) + entries + bodies
    assert len(data) == offset
    return data


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("%s\n%s%s" % (" ".join(map(str, cmd)), r.stdout, r.stderr))
    return r.stdout


def compile_c(source, out, toolchain):
    obj = out / (source.stem + ".o")
    clang = toolchain / "pi32v2" / "bin" / "clang"
    run([str(clang), *C_FLAGS, "-I", str(SHARED / "common"),
         "-c", str(source), "-o", str(obj)])
    return obj


def assemble(source, out):
    obj = out / (source.stem + ".o")
    obj.write_bytes(asm.assemble_source(source.read_text(encoding="utf-8"), filename=str(source)))
    return obj


def link(objs, origin, entry, out, name):
    script = out / f"{name}.ld"
    script.write_text('OUTPUT_FORMAT("elf32-pi32v2")\nOUTPUT_ARCH(pi32v2)\n'
                      f"ENTRY({entry})\nSECTIONS {{\n    . = 0x{origin:08X};\n"
                      "    .text : { *(.text .text.*) *(.rodata .rodata.*) }\n"
                      "    /DISCARD/ : { *(.comment) *(.note*) }\n}\n")
    path = out / f"{name}.elf"
    state, data = ld.link(ld.parse_args(["-T", str(script), "-o", str(path), *map(str, objs)]))
    if data is None or state.errors:
        raise RuntimeError("link failed: %s" % state.errors)
    path.write_bytes(data)
    return elf.read(data)


def text_of(prog, origin):
    text = next(s for s in prog.sections if s.name == ".text")
    if text.addr != origin:
        raise RuntimeError(".text at 0x%X, expected 0x%X" % (text.addr, origin))
    return bytes(text.data)


def symbol(prog, name):
    vals = [s.value for s in prog.symbols if s.name == name]
    if len(vals) != 1:
        raise RuntimeError("symbol %s not unique" % name)
    return vals[0]


def find_toolchain():
    env = os.environ.get("JIELI_TOOLCHAIN")
    if env:
        p = Path(env)
        if (p / "pi32v2" / "bin" / "clang").is_file():
            return p
    for cand in [ROOT / "toolchain", Path("/opt/jieli")]:
        if (cand / "pi32v2" / "bin" / "clang").is_file():
            return cand
    return None


def build_module(name, sources, out, tc):
    """Compile+link one module from a list of source paths; returns module dict."""
    lib = [assemble(SHARED / "common" / "h9lib.s", out)]
    objs = [compile_c(SHARED / "common" / "gfx.c", out, tc)]
    for s in sources:
        objs.append(compile_c(Path(s), out, tc))
    prog = link([*objs, *lib], LOAD, "module_main", out, name)
    body = text_of(prog, LOAD)
    if len(body) > MAX_SIZE:
        raise RuntimeError("module %s: %d bytes > %d slot" % (name, len(body), MAX_SIZE))
    return {"name": name, "body": body, "bss": 0, "entry": symbol(prog, "module_main") - LOAD}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name", help="app folder under myapps/ (e.g. bootscreen)")
    ap.add_argument("--out", default=None, help="output .h9mod path")
    ap.add_argument("--module-name", default=None, help="module name (single-module default = app name, upper)")
    args = ap.parse_args()

    appdir = ROOT / args.name
    kit_dir = appdir / "src"
    cfg_path = appdir / "build.json"

    tc = find_toolchain()
    if tc is None:
        print("No JieLi toolchain found; build inside GitHub Actions.")
        return 1

    out = ROOT / "build"
    out.mkdir(exist_ok=True)

    # A build.json lists multiple modules: [{"name":..., "sources":[...]}, ...]
    # (relative to appdir / src). Falls back to "all src/*.c as one module".
    modules = []
    if cfg_path.is_file():
        import json
        cfg = json.loads(cfg_path.read_text())
        for m in cfg["modules"]:
            mname = m["name"][:16].ljust(16)
            sources = [kit_dir / s for s in m["sources"]]
            modules.append(build_module(mname, sources, out, tc))
    else:
        sources = sorted(kit_dir.glob("*.c"))
        if not sources:
            raise RuntimeError("no .c sources in %s" % kit_dir)
        mname = (args.module_name or args.name.upper())[:16].ljust(16)
        modules.append(build_module(mname, sources, out, tc))

    data = bundle(modules)
    dst = args.out if args.out else str(appdir / (args.name + ".h9mod"))
    Path(dst).write_bytes(data)
    names = ", ".join(m["name"].rstrip() for m in modules)
    print("Built %s -> %s: %d modules (%s), %d bytes total"
          % (args.name, dst, len(modules), names, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())