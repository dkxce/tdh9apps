# SPDX-FileCopyrightText: 2026 Julius Vitkauskas
# SPDX-License-Identifier: MIT-0
"""Encode and decode single instructions from the table. The assembler and disassembler build on this."""

from .table import SYNTAX_TOKEN


def _words(data, offset, n):
    """n little-endian 16-bit words from data, as one integer (first word highest), or None if short."""
    if offset + 2 * n > len(data):
        return None
    value = 0
    for k in range(n):
        value = (value << 16) | data[offset + 2 * k] | (data[offset + 2 * k + 1] << 8)
    return value


def _to_bytes(value, n):
    out = bytearray()
    for k in range(n):
        w = (value >> (16 * (n - 1 - k))) & 0xFFFF
        out += bytes((w & 0xFF, w >> 8))
    return bytes(out)


def fields_of(insn, data, offset=0):
    """The raw field values of insn encoded at data[offset:]."""
    value = _words(data, offset, len(insn.words))
    if value is None:
        return {}
    out = {}
    for letter in insn.fields:
        v = 0
        for pos in insn.field_bits(letter):
            v = (v << 1) | ((value >> (insn.nbits - 1 - pos)) & 1)
        out[letter] = v
    return out


def decode(table, data, offset=0):
    """(insn, field values, length in bytes) for the instruction at data[offset:], or None."""
    for insn in table.insns:
        value = _words(data, offset, len(insn.words))
        if value is not None and value & insn.mask == insn.value:
            return insn, fields_of(insn, data, offset), 2 * len(insn.words)
    return None


def encode(insn, values):
    """The bytes of insn with the given raw field values."""
    value = insn.value
    for letter in insn.fields:
        bits = insn.field_bits(letter)
        v = values[letter]
        if not 0 <= v < (1 << len(bits)):
            raise ValueError(f"{insn.id}: field {letter!r} value {v} does not fit {len(bits)} bits")
        for k, pos in enumerate(bits):
            if (v >> (len(bits) - 1 - k)) & 1:
                value |= 1 << (insn.nbits - 1 - pos)
    return _to_bytes(value, len(insn.words))


SR_NAMES = ["reti", "rete", "retx", "rets", "sr4", "psr", "cnum", "uspn",
            "sspn", "icfgn", "icfgs", "icfg", "usp", "ssp", "sp", "pc"]


def _runs(regs):
    """Registers high to low, with runs of three or more written as a range: [15, 14, 13, 2] -> r15-r13, r2."""
    out, k = [], 0
    while k < len(regs):
        j = k
        while j + 1 < len(regs) and regs[j + 1] == regs[j] - 1:
            j += 1
        if j - k >= 2:
            out.append(f"r{regs[k]}-r{regs[j]}")
        else:
            out += [f"r{r}" for r in regs[k:j + 1]]
        k = j + 1
    return ", ".join(out)


def modimm(raw):
    """A 12-bit modified immediate: 8 bits repeated in a pattern, or rotated (as in ARM Thumb-2)."""
    if raw >> 10 == 0:
        xy = raw & 0xFF
        return [xy, xy * 0x00010001, xy * 0x01000100, xy * 0x01010101][raw >> 8 & 3]
    rot, byte = raw >> 7, 0x80 | (raw & 0x7F)
    return ((byte >> rot) | (byte << (32 - rot))) & 0xFFFFFFFF


def value(field, raw, width):
    """The number a uimm or simm field stands for."""
    if field.kind == "simm" and raw >= 1 << (width - 1):
        raw -= 1 << width
    if raw == 0 and field.zero_as is not None:
        return field.zero_as
    return raw * field.scale + field.add


def operand(field, raw, width):
    """How a raw field value is written in assembly."""
    kind = field.kind
    if kind in ("gpr", "gpr_lo"):
        return f"r{raw}"
    if kind == "gpr_hi":
        return f"r{raw + 8}"
    if kind == "pair":
        return f"r{2 * raw + 1}_r{2 * raw}"
    if kind == "pair_regs":         # pair number (3 bits), then one bit for each of the two names: r2,r3
        return f"r{2 * (raw >> 2) + (raw >> 1 & 1)},r{2 * (raw >> 2) + (raw & 1)}"
    if kind == "sr":
        return SR_NAMES[raw]
    if kind == "bit":
        return str(1 << raw)
    if kind == "invbit":
        return str(0xFFFFFFFF ^ (1 << raw))
    if kind == "rlist4":            # 0..3: r3 down to r<raw>; 4..15: r<raw> down to r4
        hi, lo = (3, raw) if raw < 4 else (raw, 4)
        return _runs(list(range(hi, lo - 1, -1)))
    if kind == "rmask":
        regs = _runs([r for r in range(15, -1, -1) if raw >> r & 1])
        if field.lead:
            return f"{field.lead}, {regs}" if regs else field.lead
        return regs
    if kind == "modimm":
        v = modimm(raw)
        if field.invert:
            v ^= 0xFFFFFFFF
        if field.decimal:
            return str(v)
        return f"0x{v:X}"
    if kind == "enum":
        return field.values[raw]
    if kind == "srmask":
        return ", ".join(SR_NAMES[r] for r in range(width - 1, -1, -1) if raw >> r & 1)
    return str(value(field, raw, width))


def format(insn, values, syntax=None):
    """The assembly text of insn with the given raw field values (in `syntax` if given, else insn.syntax)."""
    def sub(m):
        if m.group(0) in ("{{", "}}"):
            return m.group(0)[0]
        plus, name = m.groups()
        field = insn.fields[name]
        text = operand(field, values[name], len(insn.field_bits(name)))
        if plus:
            return "" if text == "0" else "+" + text
        return text
    return SYNTAX_TOKEN.sub(sub, insn.syntax if syntax is None else syntax)
