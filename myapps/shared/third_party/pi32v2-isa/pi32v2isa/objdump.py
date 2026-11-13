# SPDX-FileCopyrightText: 2026 Julius Vitkauskas
# SPDX-License-Identifier: MIT-0
"""An objdump for pi32v2 ELF files whose text matches JieLi's objdump (docs/OBJDUMP.md).

    python3 -m pi32v2isa objdump [-d] [-D] [-h] [-t] [-r] [-s] [-j SECTION]... [-mcpu=r3] FILE...

Disassembly goes through the instruction table (codec.decode / codec.format); everything else (headers,
labels, spacing, annotations, symbol and section tables) follows what JieLi's objdump prints, found by
comparing the two (`python3 -m pi32v2isa oracle objdumpcompare`)."""

import re
import struct
import sys
from pathlib import Path

from . import codec, elf, table as tablemod

OPTIONS_HELP = """usage: objdump [options] FILE...
  -d, -disassemble          disassemble the code sections
  -D, -disassemble-all      disassemble every section
  -h, -section-headers      print the section headers
  -t                        print the symbol table
  -r                        print the relocation records (with -d: after each symbol's code)
  -s                        print the contents of the sections
  -j NAME, -section=NAME    only these sections (repeatable)
  -mcpu=NAME                r3 and r4 enable the floating-point forms; none or r2 leave them out
  -mattr=+fprev1            enable the floating-point forms
  --chip=NAME               not in JieLi's objdump: note forms that misbehave on that chip (ac791n),
                            from the table's hw key (`; ac791n: resets the chip`)
"""

# Instruction forms JieLi's objdump decodes only when told the CPU has them (found with `oracle` probes of every
# table example, docs/OBJDUMP.md): -mcpu=r3 / -mcpu=r4 / -mattr=+fprev1 enable the floating-point ones; -mcpu=r2
# and r3 also enable the six `copex` / `shiftr ... mode` forms.
# Relocation type names objdump knows (docs/ELF.md lists the ones the assembler writes); others print "Unknown".
RELOC_NAMES = dict(elf.RELOCS)
RELOC_NAMES.update({9: "R_PI32V2_GOT", 10: "R_PI32V2_GOTFUNCDESC", 12: "R_PI32V2_FUNCDESCVALUE",
                    13: "R_PI32V2_REL32", 14: "R_PI32V2_MOV_ABS16", 15: "R_PI32V2_GOT16",
                    16: "R_PI32V2_GOTFUNCDESC16"})


class ObjdumpError(Exception):
    pass


# --- decoding ------------------------------------------------------------------------------------------------

class Decoder:
    """Instruction lookup by the first word, first matching table entry, with entries switched off by CPU."""

    def __init__(self, tab, disabled=frozenset()):
        self.tab = tab
        self.disabled = disabled
        self.by_mask = {}      # first-word mask -> {first-word value -> [(index, insn)]}
        for n, insn in enumerate(tab.insns):
            if insn.id in disabled:
                continue
            shift = 16 * (len(insn.words) - 1)
            m, v = insn.mask >> shift, insn.value >> shift
            self.by_mask.setdefault(m, {}).setdefault(v, []).append((n, insn))
        self.cache = {}
        self.memo = {}

    def candidates(self, word):
        got = self.cache.get(word)
        if got is None:
            found = []
            for m, values in self.by_mask.items():
                found += values.get(word & m, ())
            got = self.cache[word] = [i for _, i in sorted(found, key=lambda x: x[0])]
        return got

    def length_of(self, word):
        """Bytes the instruction starting with this first word needs (2, 4 or 6), as the first word decides."""
        if word < 0xE000:
            return 2
        if 0xFF00 <= word <= 0xFFEF:
            return 6
        return 4

    def decode(self, data, off):
        """(insn, values, length, text) for the instruction at data[off:], or None. The first word decides the
        length (2, 4 or 6 bytes), so the answer depends on those bytes alone and is remembered."""
        if off + 2 > len(data):
            return None
        word = data[off] | (data[off + 1] << 8)
        size = self.length_of(word)
        raw = bytes(data[off:off + size])
        if len(raw) < size:
            return None
        got = self.memo.get(raw, 0)
        if got != 0:
            return got
        got = None
        for insn in self.candidates(word):
            value = 0
            for k in range(size // 2):
                value = (value << 16) | raw[2 * k] | (raw[2 * k + 1] << 8)
            if value & insn.mask == insn.value:
                values = codec.fields_of(insn, raw)
                got = (insn, values, size, codec.format(insn, values))
                break
        self.memo[raw] = got
        return got


def decoder_for(tab, mcpu=None, mattr=()):
    """The decoder for a CPU choice, from the table's absent_on and decode_requires (no -mcpu is "generic")."""
    cpu = (mcpu or "generic").lower()
    if cpu not in SUPPORTED_CPUS:
        raise ObjdumpError(f"unsupported -mcpu={mcpu}")
    given = {a.lstrip("+") for a in mattr}
    off = frozenset(i.id for i in tab.insns if not tablemod.shown_by_objdump(i, cpu, given))
    return Decoder(tab, off)


# models whose whole encoding space was compared with JieLi's objdump; r1 and r5 read some words as other
# instructions, and the rev3 models were not swept (docs/GAPS.md)
SUPPORTED_CPUS = ("generic", "r2", "r3", "r4")


# --- reading the file ------------------------------------------------------------------------------------------

class File:
    def __init__(self, path):
        self.path = str(path)
        try:
            self.data = Path(path).read_bytes()
        except OSError as e:
            raise ObjdumpError(f"'{path}': {e.strerror}")
        if self.data[:4] != b"\x7fELF":
            raise ObjdumpError(f"'{path}': The file was not recognized as a valid object file")
        try:
            self.obj = elf.read(self.data)
        except (ValueError, IndexError, struct.error):
            raise ObjdumpError(f"'{path}': The file was not recognized as a valid object file")
        self.sections = self.obj.sections
        self.symbols = self.obj.symbols
        self.machine = self.obj.e_machine


def check_symbols(f):
    """JieLi's objdump refuses a symbol table with a symbol in a section that does not exist (-t, -d, -D)."""
    for sym in f.symbols:
        if isinstance(sym.shndx, int) and (len(f.sections) <= sym.shndx < 0xFF00 or sym.shndx == 0xFFFF):
            raise ObjdumpError(f"'{f.path}': invalid section index")


def format_name(f):
    return "ELF32-pi32v2" if f.machine == elf.EM_PI32V2 else "ELF32-unknown"


def section_kinds(s):
    """(text, data, bss): the words `-h` prints for a section and `-D` goes by (docs/OBJDUMP.md, 4.1)."""
    text = bool(s.flags & elf.SHF_EXECINSTR)
    loaded = bool(s.flags & (elf.SHF_ALLOC | elf.SHF_WRITE))
    data = s.type == elf.SHT_PROGBITS and loaded
    bss = s.type == elf.SHT_NOBITS and loaded
    return text, data, bss


# --- output pieces -----------------------------------------------------------------------------------------------

def header(f):
    return f"\n{f.path}:\tfile format {format_name(f)}\n\n"


def section_headers(f, sections):
    out = ["Sections:", "Idx Name          Size      Address          Type"]
    for k, s in enumerate(sections):
        text, data, bss = section_kinds(s)
        kinds = ("TEXT " if text else "") + ("DATA " if data else "") + ("BSS" if bss else "")
        out.append(f"{k:3d} {s.name:<13} {s.length:08x} {s.addr:016x} {kinds}")
    return "\n".join(out) + "\n"


def section_contents(f, sections):
    out = []
    for s in sections:
        if s.index == 0 or s.length == 0:
            continue
        out.append(f"Contents of section {s.name}:")
        if s.type == elf.SHT_NOBITS and section_kinds(s)[2]:
            out.append(f"<skipping contents of bss section at [{s.addr:04x}, {s.addr + s.length:04x})>")
            continue
        data = bytes(s.data) if s.type != elf.SHT_NOBITS else f.data[s.offset:s.offset + s.length]
        for off in range(0, len(data), 16):
            chunk = data[off:off + 16]
            hexs = " ".join(chunk[i:i + 4].hex() for i in range(0, len(chunk), 4))
            asc = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in chunk)
            out.append(f" {s.addr + off:04x} {hexs:<35}  {asc}")
    return "".join(x + "\n" for x in out)


def symbol_line(f, sym):
    t = sym.type
    glob = " " if t == elf.STT_NOTYPE else ("g" if sym.bind != elf.STB_LOCAL else "l")
    weak = "w" if sym.bind == elf.STB_WEAK else " "
    debug = "d" if t in (elf.STT_SECTION, elf.STT_FILE) else " "
    kind = "f" if t == elf.STT_FILE else ("F" if t == elf.STT_FUNC else " ")
    named = isinstance(sym.shndx, int) and 0 < sym.shndx < len(f.sections)
    if sym.shndx == elf.SHN_UNDEF:
        value, size = 0, (0 if t == elf.STT_COMMON else sym.size)
    elif sym.shndx == elf.SHN_COMMON:
        value, size = sym.size, sym.value
    elif t == elf.STT_COMMON:
        value, size = sym.size, 0
    else:
        value, size = sym.value, sym.size
    if sym.shndx == elf.SHN_ABS:
        where = "*ABS*"
    elif t == elf.STT_COMMON or sym.shndx == elf.SHN_COMMON:
        where = "*COM*"
    elif sym.shndx == elf.SHN_UNDEF:
        where = "*UND*"
    elif named:
        where = f.sections[sym.shndx].name
    else:
        where = "*UND*"
    name = f.sections[sym.shndx].name if t == elf.STT_SECTION and named else sym.name
    if sym.other & 3 == 2:
        name = ".hidden " + name
    return f"{value:08x} {glob}{weak}   {debug}{kind} {where}\t\t {size:08x} {name}"


def symbol_table(f):
    return "SYMBOL TABLE:\n" + "".join(symbol_line(f, s) + "\n" for s in f.symbols)


def reloc_records(f, sections):
    out = []
    for s in sections:
        if s.type != elf.SHT_RELA:
            continue
        out.append(f"RELOCATION RECORDS FOR [{s.name}]:")
        for k in range(len(s.data) // 12):
            off, info, _ = struct.unpack_from("<IIi", s.data, 12 * k)
            name = RELOC_NAMES.get(info & 0xFF, "Unknown")
            out.append(f"{off:08x} {name} Unknown")
        out.append("")
    return "\n".join(out) + "\n" if out else ""


# --- disassembly -----------------------------------------------------------------------------------------------

# Forms whose printed text carries a `<symbol+offset : address >` annotation: the branches and calls (the
# address is the end of the instruction plus the printed offset) and the immediate loads (the address is the
# value with its low bit cleared). Found by probing every table example (docs/OBJDUMP.md).
ANNOTATED_IMM = frozenset(("li.s16", "li.s16.p", "li.u32"))
ANNOTATED_BRANCH = frozenset("""
bc.cc bc.cs bc.eq bc.ge bc.gt bc.hi bc.le bc.ls bc.lt bc.ne beq.and.modimm.ri.l beq.and.ri beq.and.rr beq.ri
beq.ri.l beq.ri.l.modimm beq.rr beq.rr.l beqz bfeq.rr bfeq.rr.l bfge.rr bfge.rr.l bfgt.rr bfgt.rr.l bfle.rr
bfle.rr.l bflt.rr bflt.rr.l bfne.rr.l bford.rr.l bfueq.rr.l bfuge.rr bfuge.rr.l bfugt.rr bfugt.rr.l bfule.rr
bfule.rr.l bfult.rr bfult.rr.l bfune.rr bfune.rr.l bfunknown.rr.l bfuno.rr.l bge.ri bge.ri.l bge.ri.l.modimm
bge.rr bge.rr.l bges.ri bges.ri.l bges.ri.l.modimm bges.rr bges.rr.l bgt.ri bgt.ri.l bgt.ri.l.modimm bgt.rr
bgt.rr.l bgts.ri bgts.ri.l bgts.ri.l.modimm bgts.rr bgts.rr.l ble.ri ble.ri.l ble.ri.l.modimm ble.rr ble.rr.l
bles.ri bles.ri.l bles.ri.l.modimm bles.rr bles.rr.l blt.ri blt.ri.l blt.ri.l.modimm blt.rr blt.rr.l blts.ri
blts.ri.l blts.ri.l.modimm blts.rr blts.rr.l bne.and.modimm.ri.l bne.and.ri bne.and.rr bne.ri bne.ri.l
bne.ri.l.modimm bne.rr bne.rr.l bnez bunknown.ri.l.14to15 bunknown.ri.l.14to15.modimm bunknown.ri.l.4to7
bunknown.ri.l.4to7.modimm bunknown.rr.l.14to15 bunknown.rr.l.4to7 call.l call.s call.xl dbnz.r goto.l goto.s
gotoss.xl
""".split())
# Instructions that never close a block when they are the one that completes its body: the compare-and-branch
# forms and the vector compares. The block then stays open for good (found by probing every table example).
NO_CLOSE = frozenset("""
beq.rr bfeq.rr bne.rr bfune.rr bge.rr bfuge.rr blt.rr bflt.rr bgt.rr bfugt.rr ble.rr bfle.rr bges.rr bfge.rr
blts.rr bfult.rr bgts.rr bfgt.rr bles.rr bfule.rr all.eq.ppp.v all.ne.ppp.v all.ge.ppp.v all.lt.ppp.v
all.gt.ppp.v all.le.ppp.v alls.ge.ppp.v alls.lt.ppp.v alls.gt.ppp.v alls.le.ppp.v all.eq.rrr.v all.ne.rrr.v
all.ge.rrr.v all.lt.rrr.v all.gt.rrr.v all.le.rrr.v alls.ge.rrr.v alls.lt.rrr.v alls.gt.rrr.v alls.le.rrr.v
any.eq.ppp.v any.ne.ppp.v any.ge.ppp.v any.lt.ppp.v any.gt.ppp.v any.le.ppp.v anys.ge.ppp.v anys.lt.ppp.v
anys.gt.ppp.v anys.le.ppp.v any.eq.rrr.v any.ne.rrr.v any.ge.rrr.v any.lt.rrr.v any.gt.rrr.v any.le.rrr.v
anys.ge.rrr.v anys.lt.rrr.v anys.gt.rrr.v anys.le.rrr.v cmp.eq.ppp.v cmp.ne.ppp.v cmp.ge.ppp.v cmp.lt.ppp.v
cmp.gt.ppp.v cmp.le.ppp.v cmps.ge.ppp.v cmps.lt.ppp.v cmps.gt.ppp.v cmps.le.ppp.v cmp.eq.rrr.v cmp.ne.rrr.v
cmp.ge.rrr.v cmp.lt.rrr.v cmp.gt.rrr.v cmp.le.rrr.v cmps.ge.rrr.v cmps.lt.rrr.v cmps.gt.rrr.v cmps.le.rrr.v
beq.ri bne.ri bge.ri blt.ri bgt.ri ble.ri bges.ri blts.ri bgts.ri bles.ri beq.ri.l beq.ri.l.modimm bne.ri.l
bne.ri.l.modimm bge.ri.l bge.ri.l.modimm blt.ri.l blt.ri.l.modimm bgt.ri.l bgt.ri.l.modimm ble.ri.l
ble.ri.l.modimm bges.ri.l bges.ri.l.modimm blts.ri.l blts.ri.l.modimm bgts.ri.l bgts.ri.l.modimm bles.ri.l
bles.ri.l.modimm beq.rr.l bne.rr.l bge.rr.l blt.rr.l bgt.rr.l ble.rr.l bges.rr.l blts.rr.l bgts.rr.l bles.rr.l
bfeq.rr.l bfune.rr.l bfuge.rr.l bflt.rr.l bfugt.rr.l bfle.rr.l bfge.rr.l bfult.rr.l bfgt.rr.l bfule.rr.l
bunknown.ri.l.4to7 bunknown.rr.l.4to7 bunknown.ri.l.14to15 bunknown.rr.l.14to15 bfne.rr.l bfueq.rr.l
bfuno.rr.l bford.rr.l all.unknown4to7.ppp.v all.unknown14to15.ppp.v all.unknown4to7.rrr.v
all.unknown14to15.rrr.v any.unknown4to7.ppp.v any.unknown14to15.ppp.v any.unknown4to7.rrr.v
any.unknown14to15.rrr.v cmp.unknown4to7.ppp.v cmp.unknown14to15.ppp.v cmp.unknown4to7.rrr.v
cmp.unknown14to15.rrr.v bunknown.ri.l.4to7.modimm bunknown.ri.l.14to15.modimm bfunknown.rr.l
""".split())
# Parallel-pair first instructions that do not close a stuck block (see advance()): the vector compares.
NO_FLUSH = frozenset("""
all.eq.ppp.v.p all.ne.ppp.v.p all.ge.ppp.v.p all.lt.ppp.v.p all.gt.ppp.v.p all.le.ppp.v.p alls.ge.ppp.v.p
alls.lt.ppp.v.p alls.gt.ppp.v.p alls.le.ppp.v.p all.eq.rrr.v.p all.ne.rrr.v.p all.ge.rrr.v.p all.lt.rrr.v.p
all.gt.rrr.v.p all.le.rrr.v.p alls.ge.rrr.v.p alls.lt.rrr.v.p alls.gt.rrr.v.p alls.le.rrr.v.p any.eq.ppp.v.p
any.ne.ppp.v.p any.ge.ppp.v.p any.lt.ppp.v.p any.gt.ppp.v.p any.le.ppp.v.p anys.ge.ppp.v.p anys.lt.ppp.v.p
anys.gt.ppp.v.p anys.le.ppp.v.p any.eq.rrr.v.p any.ne.rrr.v.p any.ge.rrr.v.p any.lt.rrr.v.p any.gt.rrr.v.p
any.le.rrr.v.p anys.ge.rrr.v.p anys.lt.rrr.v.p anys.gt.rrr.v.p anys.le.rrr.v.p cmp.eq.ppp.v.p cmp.ne.ppp.v.p
cmp.ge.ppp.v.p cmp.lt.ppp.v.p cmp.gt.ppp.v.p cmp.le.ppp.v.p cmps.ge.ppp.v.p cmps.lt.ppp.v.p cmps.gt.ppp.v.p
cmps.le.ppp.v.p cmp.eq.rrr.v.p cmp.ne.rrr.v.p cmp.ge.rrr.v.p cmp.lt.rrr.v.p cmp.gt.rrr.v.p cmp.le.rrr.v.p
cmps.ge.rrr.v.p cmps.lt.rrr.v.p cmps.gt.rrr.v.p cmps.le.rrr.v.p all.unknown4to7.ppp.v.p
all.unknown14to15.ppp.v.p all.unknown4to7.rrr.v.p all.unknown14to15.rrr.v.p any.unknown4to7.ppp.v.p
any.unknown14to15.ppp.v.p any.unknown4to7.rrr.v.p any.unknown14to15.rrr.v.p cmp.unknown4to7.ppp.v.p
cmp.unknown14to15.ppp.v.p cmp.unknown4to7.rrr.v.p cmp.unknown14to15.rrr.v.p
""".split())
BRANCH_FIELD = re.compile(r"(?:goto|gotoss|call) \{(\w)\}")
TRAILING_SPACE = frozenset(("ld.w", "ld.w.pi", "ld.w.pin"))   # `r1 = [r1+0] `: the text keeps a space after `]`
HEX_BYTES = 18
M64 = 0xFFFFFFFFFFFFFFFF
BRACE_PAD = " " * 31


class Block:
    """An open `if` block: the instructions left in its then-part and else-part (a parallel pair counts as one);
    `overdue` is set when a compare-and-branch used the last of them without closing the block."""
    __slots__ = ("left", "else_left", "in_else", "overdue")

    def __init__(self, left, else_left):
        self.left, self.else_left, self.in_else, self.overdue = left, else_left, False, False


class Symbols:
    """The symbols an address is named by: `name+offset`, as the nearest symbol at or below it."""

    def __init__(self, f):
        self.rel = f.obj.e_type == 1
        pairs = []
        for x in f.symbols[1:]:
            if (not x.name or x.shndx == elf.SHN_UNDEF or x.shndx == elf.SHN_COMMON or x.type == elf.STT_SECTION
                    or x.name.startswith(".L")):
                continue
            pairs.append((x.size if x.type == elf.STT_COMMON else x.value, x.name, x.shndx))
        self.all = sorted(pairs, key=lambda p: (p[0], p[1]))
        self.addrs = [p[0] for p in self.all]
        self.by_section = {}
        self.section_addrs = {}

    def set_section(self, index, pairs):
        """The names an address in section `index` is looked up in (relocatable files): its own symbols."""
        self.by_section[index] = [(a, n, index) for a, n, _ in pairs]
        self.section_addrs[index] = [a for a, _, _ in pairs]

    def lookup(self, target, section):
        import bisect
        if self.rel:
            lst = self.by_section.get(section, [])
            addrs = self.section_addrs[section]
        else:
            lst, addrs = self.all, self.addrs
        k = bisect.bisect_right(addrs, target) - 1
        if k < 0:
            return None
        return lst[k]

    def annotation(self, target, section):
        target &= M64
        got = self.lookup(target, section)
        if got is None:
            return ""
        off = target - got[0]
        return f" <{got[1]}{'+0x%X' % off if off else ''} : {target:x} >"


class Disassembler:
    def __init__(self, f, dec, opts):
        self.f = f
        self.dec = dec
        self.opts = opts
        self.size = 1                 # the length the decoder last reported: it stays when a decode fails early
        self.rep = None               # open blocks and the pair state go on across labels and sections
        self.cond = None
        self.second = False
        self.reach = 0
        self.out = []
        self.syms = Symbols(f)

    def run(self, sections):
        for s in sections:
            self.section(s)
        return "".join(self.out)

    def section(self, s):
        f = self.f
        w = self.out.append
        text, is_data, _ = section_kinds(s)
        if s.length == 0 or (s.type == elf.SHT_NOBITS and not self.opts.get("D")):
            return
        w(f"Disassembly of section {s.name}:\n")
        exec_section = text
        syms = [(x.size if x.type == elf.STT_COMMON else x.value, x.name, x.type) for x in f.symbols
                if x.shndx == s.index and x.type != elf.STT_SECTION and x.name and not x.name.startswith(".L")]
        pairs = sorted(syms, key=lambda p: (p[0], p[1]))
        start = s.addr
        if not pairs or pairs[0][0] != start:
            pairs.insert(0, (start, s.name, elf.STT_NOTYPE))
        self.syms.set_section(s.index, pairs)
        data = bytes(s.data) if s.type != elf.SHT_NOBITS else self.f.data[s.offset:s.offset + s.length]
        size = s.length
        first = True
        rel = list(s.relocs)
        for k, (addr, name, typ) in enumerate(pairs):
            lo = addr - s.addr
            hi = size if k + 1 == len(pairs) else pairs[k + 1][0] - s.addr
            hi = min(hi, size)
            if lo < 0 or lo >= hi:                     # a symbol below the section, or an empty block
                continue
            table_entry = 0                        # compiler jump tables: .GJTIS (bytes), .GJTIL.. (halfwords)
            if name.startswith(".GJTI") and len(name) > 5 and name[5] != "E":
                table_entry = 1 if name[5] == "S" else 2
            if name.startswith(".GJTIE"):
                pass                               # the end of a table: code goes on with no label
            else:
                w(("" if first else "\n") + f"{name}:\n")
            first = False
            self.reach = 0                         # relocation records below this offset belong to the block
            if not exec_section:                   # -D on a data section: text in 4-byte pieces
                if is_data:
                    self.strings(s, data, lo, hi)
            elif table_entry:
                self.table(s, lo, hi, table_entry)
            else:
                if typ == elf.STT_OBJECT and not self.opts.get("D"):
                    self.hexdump(s, data, lo, hi)
                    self.reach = hi + 1
                else:
                    self.code(s, data, lo, hi)
                w("\n")
                if self.opts.get("r"):
                    while rel and rel[0][0] < self.reach:
                        off, rtype = rel.pop(0)[:2]
                        w(f"\t\t\t{off:08x}:  {RELOC_NAMES.get(rtype, 'Unknown')}\tUnknown\n")

    @staticmethod
    def escape(b):
        simple = {0: "\\0", 8: "\\b", 9: "\\t", 10: "\\n", 13: "\\r"}
        if b in simple:
            return simple[b]
        return chr(b) if 0x20 <= b <= 0x7E else f"\\x{b & 15:02x}"

    def strings(self, s, data, lo, hi):
        w = self.out.append
        for a in range(lo, hi, 4):
            chunk = data[a:min(a + 4, hi)]
            w(f"{s.addr + a:8x}:    {self.hexb(chunk):<16}\"{''.join(self.escape(b) for b in chunk)}\"\n")

    def table(self, s, lo, hi, step):
        w = self.out.append
        for a in range(lo, hi, step):
            raw = self.raw(s, a, step)
            v = int.from_bytes(raw, "little")
            w(f"{s.addr + a:8x}:    {' '.join(f'{b:02x}' for b in raw)}     < {v} : 0x{v:X} >\n")
            self.size = step

    def hexdump(self, s, data, lo, hi):
        w = self.out.append
        for a in range(lo, hi, 8):
            chunk = data[a:min(a + 8, hi)]
            hexs = "".join(f" {b:02x}" for b in chunk)
            asc = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in chunk)
            w(f"{s.addr + a:8x}:\t{hexs:<33}{asc}\n")

    def raw(self, s, off, n):
        """n bytes at section offset off, taken from the file (so also past the section's end)."""
        base = s.offset + off
        return self.f.data[base:base + n]

    @staticmethod
    def hexb(raw):
        return raw.hex(" ") + " " if raw else ""

    def code(self, s, data, lo, hi):
        w = self.out.append
        index = lo
        while index < hi:
            second = self.second
            addr = s.addr + index
            got = self.dec.decode(data, index)
            if got is None:
                if index + 2 <= len(data):      # a word to read: 2 bytes if the instruction fits, else 1
                    fits = index + self.dec.length_of(data[index] | (data[index + 1] << 8)) <= len(data)
                    self.size = 2 if fits else 1
                n = self.size
                w(f"{addr:8x}:    {self.hexb(self.raw(s, index, n)):<{HEX_BYTES}} <unknown instruction>\n")
                index += n
                self.reach = index + n
                continue
            insn, values, n, text = got
            self.size = n
            word = data[index] | (data[index + 1] << 8)
            if insn.id in TRAILING_SPACE:
                text += " "
            depth = (self.rep is not None) + (self.cond is not None)
            if second:      # the second of a parallel pair: one more tab and a space, but two spaces in an if block
                pad = "\t" * (depth + 1) + ("  " if self.cond is not None else "\t ")
            else:
                pad = "\t" * (depth + 1)
            first_of_pair = 0xC000 <= word <= 0xDFFF or 0xF000 <= word <= 0xF7FF
            closers = self.advance(insn, values, n, second, first_of_pair)
            if closers and insn.body:      # a `rep` that closes a block puts its `{` after the `}`
                text = text[:-2]
                closers[-1] = closers[-1].replace("\0", "") + " {"
            ann = ""
            if insn.id in ANNOTATED_BRANCH:
                m = BRANCH_FIELD.search(insn.syntax)
                ann = self.syms.annotation((addr + n + codec.value(insn.fields[m.group(1)], values[m.group(1)],
                                            len(insn.field_bits(m.group(1))))) & ~1, s.index)
            elif insn.id in ANNOTATED_IMM:
                v = int(re.search(r"= (-?\d+)", text).group(1))
                ann = self.syms.annotation(v & ~1, s.index)
            chip = self.opts.get("chip")
            note = chip and tablemod.hw_note(insn, chip)
            if note:
                text += f"\t; {chip}: {note}"
            w(f"{addr:8x}:    {self.hexb(data[index:index + n]):<{HEX_BYTES}}{pad}{text}")
            for c in closers:       # a rep's closer has a tab even after the second of a pair
                w("\n" + (" " * 32 if second and not c.endswith("\0") else BRACE_PAD + "\t") + c.replace("\0", ""))
            w(ann + "\n")
            self.second = first_of_pair
            index += n
            self.reach = index + n

    def advance(self, insn, values, n, second, first_of_pair):
        """Account for one printed instruction in the open blocks; return the brace lines it closes. There is
        at most one `rep` (its body counted in bytes) and one `if` (counted in instructions, a parallel pair as
        one) open at a time; a new one replaces the open one of its kind. The first instruction of a parallel
        pair leaves any closing to the second one. A compare-and-branch that completes an `if` body does not
        close it; the block then stays open until a parallel pair starts, which closes it."""
        cond = self.cond
        closers = []

        def finish_cond():
            if not cond.in_else and cond.else_left > 0:
                cond.in_else = True
                closers.append("} else {")
            else:
                self.cond = None
                closers.append("}")

        if (cond is not None and cond.overdue and first_of_pair and not second and insn.id not in NO_FLUSH
                and (cond.else_left if cond.in_else else cond.left) == 0):
            finish_cond()
            cond.overdue = False
        if self.rep is not None:
            self.rep -= n
        if cond is not None and not second:
            if cond.in_else:
                cond.else_left -= 1
            else:
                cond.left -= 1
        if not first_of_pair and self.cond is not None and not insn.block:    # an `if` opener replaces the open one
            done = cond.else_left == 0 if cond.in_else else cond.left == 0
            if done and insn.id in NO_CLOSE:
                cond.overdue = True
            elif done:
                finish_cond()
        if self.rep is not None and self.rep <= 0:
            self.rep = None
            closers.append("}\0")
        if insn.body:
            f = insn.fields[insn.body]
            self.rep = codec.value(f, values[insn.body], len(insn.field_bits(insn.body)))
        elif insn.block:
            def cnt(key):
                f = insn.fields[insn.block[key]]
                return codec.value(f, values[insn.block[key]], len(insn.field_bits(insn.block[key])))
            self.cond = Block(cnt("then"), cnt("else"))
        return closers


def run_file(f, opts, tab):
    out = [header(f)]
    wanted = opts.get("sections")
    secs = [s for s in f.sections if not wanted or s.name in wanted]
    if opts.get("d") or opts.get("D") or opts.get("t"):
        check_symbols(f)
    if opts.get("d") or opts.get("D"):
        dec = decoder_for(tab, opts.get("mcpu"), opts.get("mattr", ()))
        code = [s for s in secs if opts.get("D") or s.flags & elf.SHF_EXECINSTR]
        out.append(Disassembler(f, dec, opts).run(code))
    if opts.get("r") and not (opts.get("d") or opts.get("D")):
        out.append(reloc_records(f, secs))
    if opts.get("h"):
        out.append(section_headers(f, secs))
    if opts.get("s"):
        out.append(section_contents(f, secs))
    if opts.get("t"):
        out.append(symbol_table(f))
    return "".join(out)


def _missing(option):
    raise ObjdumpError(f"option {option} needs a value")


def parse_args(argv):
    opts = {"files": [], "sections": [], "mattr": []}
    it = iter(argv)
    for a in it:
        if a in ("-d", "-disassemble", "--disassemble"):
            opts["d"] = True
        elif a in ("-D", "-disassemble-all", "--disassemble-all"):
            opts["D"] = True
        elif a in ("-h", "-section-headers", "--section-headers", "-headers"):
            opts["h"] = True
        elif a in ("-t", "--syms"):
            opts["t"] = True
        elif a in ("-r", "--reloc"):
            opts["r"] = True
        elif a in ("-s", "--full-contents"):
            opts["s"] = True
        elif a == "-j" or a == "-section":
            opts["sections"].append(next(it, None) or _missing(a))
        elif a.startswith("-j") and len(a) > 2:
            opts["sections"].append(a[2:])
        elif a.startswith(("-section=", "--section=")):
            opts["sections"].append(a.split("=", 1)[1])
        elif a.startswith("-mcpu="):
            opts["mcpu"] = a.split("=", 1)[1]
        elif a.startswith("-mattr="):
            opts["mattr"] += a.split("=", 1)[1].split(",")
        elif a.startswith("--chip="):
            opts["chip"] = a.split("=", 1)[1]
            if opts["chip"] not in tablemod.CHIPS:
                raise ObjdumpError(f"unknown chip {opts['chip']} (known: {', '.join(tablemod.CHIPS)})")
        elif a.startswith("-"):
            raise ObjdumpError(f"unsupported option {a}")
        else:
            opts["files"].append(a)
    return opts


def main(argv=None, tab=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    try:
        opts = parse_args(argv)
        if not opts["files"]:
            sys.stderr.write(OPTIONS_HELP)
            return 1
        tab = tab or tablemod.load()
        for path in opts["files"]:
            sys.stdout.write(run_file(File(path), opts, tab))
    except ObjdumpError as e:
        sys.stderr.write(f"objdump: {e}\n")
        return 1
    return 0
