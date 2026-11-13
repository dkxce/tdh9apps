# SPDX-FileCopyrightText: 2026 Julius Vitkauskas
# SPDX-License-Identifier: MIT-0
"""ELF32 relocatable objects for pi32v2: a writer for what the assembler produces and a reader for comparing
objects (docs/ELF.md describes the layout JieLi's assembler writes, which this follows)."""

import struct
from dataclasses import dataclass, field

EM_PI32V2 = 0xF1

SHT_NULL, SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_RELA, SHT_NOBITS = 0, 1, 2, 3, 4, 8
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR, SHF_MERGE, SHF_STRINGS, SHF_INFO_LINK = 1, 2, 4, 0x10, 0x20, 0x40
SHF_GROUP, SHF_TLS = 0x200, 0x400
STB_LOCAL, STB_GLOBAL, STB_WEAK = 0, 1, 2
STT_NOTYPE, STT_OBJECT, STT_FUNC, STT_SECTION, STT_FILE, STT_COMMON, STT_TLS = 0, 1, 2, 3, 4, 5, 6
SHN_UNDEF, SHN_ABS, SHN_COMMON = 0, 0xFFF1, 0xFFF2

# Relocation types JieLi's assembler writes (docs/ELF.md): number -> (name, bytes of the patched item)
RELOCS = {
    0: "R_PI32V2_NONE",
    1: "R_PI32V2_LONG_JUMP_17M2",
    2: "R_PI32V2_LONG_JUMP_23M2",
    3: "R_PI32V2_EXTEND_JUMP_17M2",
    4: "R_PI32V2_EXTEND_CALL_32M2",
    5: "R_PI32V2_MOV_ABS32",
    6: "R_PI32V2_ABS8",
    7: "R_PI32V2_ABS16",
    8: "R_PI32V2_ABS32",
    11: "R_PI32V2_FUNCDESC",
}
RELOC_NUMBERS = {name: number for number, name in RELOCS.items()}


@dataclass
class Section:
    name: str
    type: int = SHT_PROGBITS
    flags: int = 0
    align: int = 1
    entsize: int = 0
    data: bytearray = field(default_factory=bytearray)   # contents (NOBITS: only the size matters)
    size: int = 0                                        # NOBITS size
    addr: int = 0                                        # sh_addr (read from linked files)
    offset: int = 0                                      # sh_offset (read from files)
    relocs: list = field(default_factory=list)           # (offset, type, symbol name, addend)
    index: int = 0

    @property
    def length(self):
        return self.size if self.type == SHT_NOBITS else len(self.data)


@dataclass
class Symbol:
    name: str
    value: int = 0
    size: int = 0
    bind: int = STB_LOCAL
    type: int = STT_NOTYPE
    other: int = 0
    shndx: object = SHN_UNDEF   # section index, SHN_ABS, SHN_COMMON, SHN_UNDEF, or (for write) a section name


def _align(n, a):
    return (n + a - 1) // a * a


class StringTable:
    """A string table laid out like JieLi's assembler does: the strings sorted by their reversed spelling,
    largest first, with a string that ends the previous one sharing its bytes."""

    def __init__(self):
        self.names = set()

    def add(self, name):
        if name:
            self.names.add(name)

    def layout(self):
        """(bytes, {name: offset})."""
        out, where, prev = bytearray(b"\0"), {"": 0}, None
        for name in sorted(self.names, key=lambda n: n.encode()[::-1], reverse=True):
            if prev is not None and prev.endswith(name):
                where[name] = where[prev] + len(prev) - len(name)
                continue
            where[name] = len(out)
            out += name.encode() + b"\0"
            prev = name
        return bytes(out), where


def write(sections, symbols, e_flags=0):
    """Serialise an object. `sections`: Section list (user sections in order; relocations are in each section's
    `relocs` with symbol names). `symbols`: Symbol list in the order they go in .symtab (locals first, then the
    rest); the first local entry is added here. Layout like JieLi's: header, section contents, .symtab, the
    .rela sections, .strtab, section headers; section 1 is .strtab and serves as the section name table."""
    rela = [s for s in sections if s.relocs]
    names = StringTable()
    for s in sections:
        names.add(s.name)
    for s in rela:
        names.add(".rela" + s.name)
    names.add(".strtab")
    names.add(".symtab")
    for sym in symbols:
        names.add(sym.name)
    strtab, where = names.layout()

    # section numbering: 0 null, 1 .strtab, then each user section followed by its .rela section, .symtab last
    rela_index, index = {}, 2
    for s in sections:
        s.index = index
        index += 1
        if s.relocs:
            rela_index[id(s)] = index
            index += 1
    symtab_index = index
    by_name = {s.name: s.index for s in sections}
    symbol_number = {sym.name: n + 1 for n, sym in enumerate(symbols)}
    nlocal = 1 + sum(1 for sym in symbols if sym.bind == STB_LOCAL)

    body = bytearray()
    offsets = {}
    pos = 52

    def place(key, data, align):
        nonlocal pos
        pad = _align(pos, align) - pos
        body.extend(b"\0" * pad)
        pos += pad
        offsets[key] = pos
        body.extend(data)
        pos += len(data)

    for s in sections:
        place(id(s), b"" if s.type == SHT_NOBITS else bytes(s.data), s.align)
        if s.type == SHT_NOBITS:
            pass
    symdata = bytearray(16)
    for sym in symbols:
        symdata += struct.pack("<IIIBBH", where[sym.name], sym.value, sym.size,
                               (sym.bind << 4) | sym.type, sym.other,
                               by_name[sym.shndx] if isinstance(sym.shndx, str) else sym.shndx)
    place("symtab", bytes(symdata), 4)
    for s in rela:
        data = bytearray()
        for offset, rtype, symname, addend in s.relocs:
            # Compiler assembly can spell -1 as +4294967295; Elf32_Rela
            # stores the same low 32 bits in its signed addend field.
            addend = ((addend + (1 << 31)) & 0xFFFFFFFF) - (1 << 31)
            data += struct.pack("<IIi", offset, (symbol_number[symname] << 8) | rtype, addend)
        place(("rela", id(s)), bytes(data), 4)
    place("strtab", strtab, 1)
    shoff = _align(pos, 4)
    body.extend(b"\0" * (shoff - pos))

    headers = [struct.pack("<10I", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    headers.append(struct.pack("<10I", where[".strtab"], SHT_STRTAB, 0, 0, offsets["strtab"], len(strtab), 0, 0, 1, 0))
    for s in sections:
        off = offsets[id(s)]
        headers.append(struct.pack("<10I", where[s.name], s.type, s.flags, 0, off, s.length, 0, 0, s.align, s.entsize))
        if s.relocs:
            headers.append(struct.pack("<10I", where[".rela" + s.name], SHT_RELA, 0, 0, offsets[("rela", id(s))],
                                       12 * len(s.relocs), symtab_index, s.index, 4, 12))
    headers.append(struct.pack("<10I", where[".symtab"], SHT_SYMTAB, 0, 0, offsets["symtab"], len(symdata),
                               1, nlocal, 4, 16))
    for h in headers:
        body.extend(h)
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    header = ident + struct.pack("<HHIIIIIHHHHHH", 1, EM_PI32V2, 1, 0, 0, shoff, e_flags, 52, 0, 0, 40,
                                 len(headers), 1)
    return header + bytes(body)


# --- reading -------------------------------------------------------------------------------------------------

@dataclass
class Obj:
    e_machine: int
    e_flags: int
    sections: list      # Section, in file order (index 0 is the null section)
    symbols: list       # Symbol, symtab order (index 0 is the null symbol)
    e_type: int = 1     # ET_REL (1), ET_EXEC (2)
    entry: int = 0


def read(data):
    """Parse an ELF32 little-endian relocatable object into an Obj. Relocations are attached to their target
    section as (offset, type, symbol name, addend); REL entries get addend None."""
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ValueError("not a little-endian ELF32 file")
    (e_type, machine, _, entry, _, shoff, flags, _, _, _, shentsize, shnum, shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    raw = [struct.unpack_from("<10I", data, shoff + k * shentsize) for k in range(shnum)]

    def cstr(off, base):
        end = data.index(b"\0", base + off)
        return data[base + off:end].decode()

    strbase = raw[shstrndx][4] if shnum else 0
    sections = []
    for k, (name, typ, flg, addr, off, size, link, info, align, entsize) in enumerate(raw):
        s = Section(cstr(name, strbase) if k else "", typ, flg, align, entsize)
        s.index, s.addr, s.offset = k, addr, off
        if typ == SHT_NOBITS:
            s.size = size
        elif typ != SHT_NULL:
            s.data = bytearray(data[off:off + size])
        sections.append(s)
    symbols = []
    for k, (name, typ, flg, _, off, size, link, info, align, entsize) in enumerate(raw):
        if typ == SHT_SYMTAB:
            sb = raw[link][4]
            for n in range(size // 16):
                nm, value, sz, infob, other, shndx = struct.unpack_from("<IIIBBH", data, off + 16 * n)
                symbols.append(Symbol(cstr(nm, sb), value, sz, infob >> 4, infob & 15, other, shndx))
    for k, (name, typ, flg, _, off, size, link, info, align, entsize) in enumerate(raw):
        if typ in (SHT_RELA, 9):
            step = 12 if typ == SHT_RELA else 8
            for n in range(size // step):
                if typ == SHT_RELA:
                    o, i, a = struct.unpack_from("<IIi", data, off + 12 * n)
                else:
                    (o, i), a = struct.unpack_from("<II", data, off + 8 * n), None
                sections[info].relocs.append((o, i & 0xFF, symbols[i >> 8].name, a))
    return Obj(machine, flags, sections, symbols, e_type, entry)
