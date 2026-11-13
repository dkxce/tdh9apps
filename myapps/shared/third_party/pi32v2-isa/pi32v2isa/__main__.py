# SPDX-FileCopyrightText: 2026 Julius Vitkauskas
# SPDX-License-Identifier: MIT-0
"""Command line: python3 -m pi32v2isa check | disasm HEX... | stats | as | ld | oracle"""

import argparse
from pathlib import Path
import signal
import sys
import collections
from collections import Counter

from . import codec, table


def cmd_check(args):
    t = table.load(args.table)
    errors = table.check(t)
    for e in errors:
        print(e, file=sys.stderr)
    print(f"{len(t.insns)} entries, {len(errors)} problems")
    return 1 if errors else 0


def cmd_disasm(args):
    t = table.load(args.table)
    data = bytes(int(b, 16) for b in " ".join(args.hex).split())
    offset = 0
    while offset < len(data):
        got = codec.decode(t, data, offset)
        if got is None:
            n = min(2, len(data) - offset)
            text = ".short 0x" + data[offset:offset + n][::-1].hex()
        else:
            insn, values, n = got
            text = codec.format(insn, values)
        print(f"{args.base + offset:8x}:  {data[offset:offset + n].hex(' '):<18} {text}")
        offset += n
    return 0


def cmd_stats(args):
    t = table.load(args.table)
    by_status = Counter(i.status for i in t.insns)
    print(f"{len(t.insns)} entries: " + ", ".join(f"{by_status[s]} {s}" for s in table.STATUSES))
    return 0


def cmd_as(args):
    from . import asm

    return asm.main(args)


def cmd_oracle(args):
    from . import oracle

    if args.action == "fetch":
        print(oracle.fetch(args.release))
        return 0
    o = oracle.Oracle(args.release)
    if args.action == "decode":
        data = bytes(int(b, 16) for b in " ".join(args.items).split())
        for addr, got, text in o.listing(data, args.feature):
            print(f"{addr:8x}:  {got.hex(' '):<18} {text}")
        return 0
    if args.action == "encode":
        for text, got in zip(args.items, o.encode(args.items, args.feature)):
            print(f"{text}\t" + (f"error: {got}" if isinstance(got, str) else got.hex(" ")))
        return 0
    if args.action == "vary":
        template, *specs = args.items
        values = {k: oracle.parse_values(v) for k, _, v in (x.partition("=") for x in specs)}
        for text, got in oracle.vary(o, template, values, args.feature):
            if isinstance(got, str):
                print(f"{text:<36} error: {got.splitlines()[0] if got else '?'}")
            else:
                print(f"{text:<36} {got.hex(' '):<18} {oracle.bits(got)}")
        return 0
    if args.action == "sweep":
        first = int(args.items[0], 16) if args.items else None
        for shape, n, ws, pattern in oracle.shapes(oracle.sweep(o, first)):
            print(f"{len(ws):6d}  {n or '-'}  {pattern}  {shape or '(nothing)'}")
        return 0
    if args.action == "fit":
        regex, *rest = args.items
        first = int(rest[0], 16) if rest else None
        for pat, n, complete, roles in oracle.fit(oracle.sweep(o, first), regex, args.split):
            print(f"{pat}  {n} words{'' if complete else '  (holes: other shapes inside)'}")
            for b, deltas in roles:
                print(f"   bit {b:2d}: " + ", ".join(f"{d} x{c}" for d, c in deltas))
        return 0
    if args.action == "cc":
        source = Path(args.items[0]).read_text() if args.items and args.items[0] != "-" else sys.stdin.read()
        print(oracle.compile_c(o, source, args.flag, args.feature), end="")
        return 0
    if args.action == "coverage":
        lo, _, hi = (args.items[0] if args.items else "0-ffff").partition("-")
        total, covered, missing = oracle.coverage(o, table.load(args.table), int(lo, 16), int(hi or lo, 16))
        print(f"{covered} of {total} decodable words covered; missing shapes:")
        for shape, n, ws, pattern in missing:
            print(f"{len(ws):6d}  {n}  {pattern}  {shape}")
        return 0
    if args.action == "compare":
        t = table.load(args.table)
        total = collections.Counter()
        for obj in args.items:
            counts, diffs = oracle.compare(o, t, obj)
            total.update(counts)
            for d in diffs[:args.show]:
                print(d)
        print(", ".join(f"{n} {k}" for k, n in total.items()) or "no code found")
        return 1 if total["different"] or total["not in table"] else 0
    if args.action == "linkcompare":
        t = table.load(args.table)
        bad = 0
        for path in args.items:
            diffs = oracle.linkcompare(o, path, args.flag, args.include, args.define, t, args.feature)
            print(f"{path}: " + ("linked sections identical" if not diffs else f"{len(diffs)} differences"))
            for d in diffs:
                print(f"  {d}")
            bad += bool(diffs)
        return 1 if bad else 0
    if args.action == "ldcompare":
        diffs, ref, mine = oracle.ldcompare(o, args.items, cwd=args.cwd)
        if ref is None and not diffs:
            print("both linkers fail")
            return 0
        extra = []
        if ref is not None and mine is not None:
            for name in sorted({s["name"] for s in oracle.read_exec(ref)["sections"] if s["flags"] & 2 and s["type"] == 1}):
                if oracle.objcopy_text(o, ref, name) != oracle.objcopy_text(o, mine, name):
                    extra.append(("contents", f"objcopy -O binary -j {name} differs"))
        diffs = diffs + extra
        if not diffs:
            print("identical" if ref == mine else "loaded contents, symbols, program headers and layout identical")
            return 0
        cats = {}
        for cat, text in diffs:
            cats.setdefault(cat, []).append(text)
        for cat in ("contents", "symbols", "entry", "phdrs", "layout"):
            if cat in cats:
                print(f"{cat}: {len(cats[cat])} differences")
                for t in cats[cat][:args.show * 4]:
                    print(f"  {t}")
        return 1
    if args.action == "asmcompare":
        t = table.load(args.table)
        bad = 0
        for path in args.items:
            diffs = oracle.asmcompare(o, path, args.flag, args.include, args.define, t, args.feature)
            print(f"{path}: " + ("identical" if not diffs else f"{len(diffs)} differences"))
            for d in diffs[:args.show * 4]:
                print(f"  {d}")
            bad += bool(diffs)
        return 1 if bad else 0
    # confirm
    t = table.load(args.table)
    ids = args.items or [i.id for i in t.insns if i.status != "confirmed"]
    failed = 0
    for insn_id in ids:
        insn = t.by_id(insn_id)
        problems, examples, checked, preferred = oracle.confirm(o, t, insn)
        if problems:
            failed += 1
            print(f"{insn_id}: {len(problems)} disagreements over {checked} encodings")
            for p in problems[:args.show]:
                print(f"  {p}")
            if len(problems) > args.show:
                print(f"  ... {len(problems) - args.show} more")
            continue
        own = preferred.pop(oracle.SELF, None)
        also = "; assembler prefers " + ", ".join(f"{k} ({n})" for k, n in preferred.items()) if preferred else ""
        if own == 0 and preferred:
            also += "; it never picks this form itself"
        print(f"{insn_id}: {checked} encodings agree both ways{also}")
        if args.write:
            table.record_confirmation(args.table, insn_id, examples)
    if args.write:
        errors = table.check(table.load(args.table))
        for e in errors:
            print(e, file=sys.stderr)
        if errors:
            return 1
    return 1 if failed else 0


STANDARD_OBJDUMP_SETS = [["-d"], ["-d", "-mcpu=r3"], ["-d", "-r"], ["-D"], ["-h"], ["-t"], ["-r"], ["-s"]]


def cmd_objdumpcompare(argv):
    """oracle objdumpcompare FILE... [objdump options]: JieLi's objdump against ours, line by line. Without
    options every standard set (-d, -d -mcpu=r3, -d -r, -D, -h, -t, -r, -s) is run. --show N, --release R."""
    from . import oracle

    files, opts, show, release = [], [], 8, oracle.DEFAULT_RELEASE
    it = iter(argv)
    for a in it:
        if a == "--show":
            show = int(next(it))
        elif a == "--release":
            release = next(it)
        elif a in ("-j", "-section"):
            opts += [a, next(it, "")]
        elif a.startswith("-"):
            opts.append(a)
        else:
            files.append(a)
    o = oracle.Oracle(release)
    t = table.load()
    bad = 0
    for path in files:
        for sets in ([opts] if opts else STANDARD_OBJDUMP_SETS):
            diffs = oracle.objdumpcompare(o, path, sets, t)
            print(f"{path} {' '.join(sets)}: " + ("identical" if not diffs else f"{len(diffs)} differences"))
            for d in diffs[:show]:
                print(f"  {d}")
            bad += bool(diffs)
    return 1 if bad else 0


def main(argv=None):
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)     # quiet when piped into head
    if (sys.argv[1:] if argv is None else argv)[:2] == ["oracle", "objdumpcompare"]:
        return cmd_objdumpcompare((sys.argv[1:] if argv is None else argv)[2:])
    if (sys.argv[1:] if argv is None else argv)[:1] == ["objdump"]:     # its options are objdump's, not argparse's
        from . import objdump

        return objdump.main((sys.argv[1:] if argv is None else argv)[1:])

    raw = sys.argv[1:] if argv is None else list(argv)
    if raw and raw[0] == "ld":                        # the linker takes ld's own options: argparse must not see them
        from . import ld

        return ld.main(argparse.Namespace(argv=raw[1:]))
    p = argparse.ArgumentParser(prog="pi32v2isa")
    p.add_argument("--table", default=table.TABLE, help="instruction table (default: isa/pi32v2.toml)")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check", help="validate the table").set_defaults(func=cmd_check)
    d = sub.add_parser("disasm", help="decode hex bytes")
    d.add_argument("hex", nargs="+", help="bytes in hex, e.g. 80 00 01 00")
    d.add_argument("--base", type=lambda s: int(s, 0), default=0, help="address of the first byte")
    d.set_defaults(func=cmd_disasm)
    sub.add_parser("stats", help="count entries by status").set_defaults(func=cmd_stats)
    sub.add_parser("objdump", help="objdump FILE... [-d -D -h -t -r -s -j SECTION -mcpu=r3] (docs/OBJDUMP.md)")

    sub.add_parser("ld", help="link pi32v2 objects with a linker script (docs/LINKER.md); options as GNU ld's")
    a = sub.add_parser("as", help="assemble a pi32v2 source file into an ELF object (docs/ASSEMBLER.md)")
    a.add_argument("input", help="assembly source (- for stdin); .S files go through cpp")
    a.add_argument("-o", "--output", help="object file to write (default: the input's name with .o)")
    a.add_argument("--cpp", action="store_true", help="run /usr/bin/cpp over the input first")
    a.add_argument("-I", dest="include", action="append", default=[], help="include/incbin directory (also passed to cpp)")
    a.add_argument("-D", dest="define", action="append", default=[], help="macro definition for cpp")
    a.add_argument("-m", dest="feature", action="append", default=[],
                   help="CPU feature the code may use (instrev3, fprev1); without any -m every instruction is allowed")
    a.add_argument("--chip", choices=sorted(table.CHIPS),
                   help="warn about forms that reset, do nothing, run as another form or are unpredictable on this "
                        "chip (the table's hw key)")
    a.add_argument("--chip-errors", action="store_true", help="with --chip: refuse those forms instead of warning")
    a.set_defaults(func=cmd_as)
    o = sub.add_parser("oracle", help="use JieLi's toolchain as a black-box oracle (docs/ORACLE.md)")
    o.add_argument("action", choices=["fetch", "decode", "encode", "vary", "sweep", "fit", "coverage", "compare", "cc", "confirm", "asmcompare", "linkcompare", "ldcompare"])
    o.add_argument("items", nargs="*", help="decode: hex bytes; encode: lines; vary: TEMPLATE name=values...; sweep: [FIRST WORD]; fit: REGEX [FIRST WORD]; coverage: [LO-HI hex]; cc: FILE.c or - ; compare: objects; confirm: ids; asmcompare: FILE.S (or .s, .c) [more files]; ldcompare: -- followed by ld's arguments (objects, -T script, options)")
    o.add_argument("--release", default="20260730.1", help="toolchain release")
    o.add_argument("--feature", action="append", default=[], help="extra -m feature, e.g. fprev1")
    o.add_argument("--write", action="store_true", help="confirm: record passing entries in the table")
    o.add_argument("--show", type=int, default=8, help="confirm: problems to show per entry")
    o.add_argument("--flag", action="append", default=[], help="cc: extra clang flag, e.g. --flag=-O2")
    o.add_argument("-I", dest="include", action="append", default=[], help="asmcompare: include directory")
    o.add_argument("-D", dest="define", action="append", default=[], help="asmcompare: cpp macro definition")
    o.add_argument("--cwd", default=None, help="ldcompare: directory to link in (relative object and script paths)")
    o.add_argument("--split", type=lambda v: int(v, 0), default=0, help="fit: bits to group the words by")
    o.set_defaults(func=cmd_oracle)
    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
