# SPDX-FileCopyrightText: 2026 Julius Vitkauskas
# SPDX-License-Identifier: MIT-0
"""A table-driven pi32v2 assembler that writes ELF relocatable objects (docs/ASSEMBLER.md, docs/ELF.md).

Instruction matchers are built from the table's `syntax` and `asm_syntax` strings by inverting
`codec.format`/`codec.operand`; encodings are chosen like JieLi's assembler does (the shortest form whose fields
fit, branches relaxed to a fixed point). Directives follow the LLVM-style assembler JieLi's toolchain contains.
"""

import os
import re
import struct
import subprocess
import sys
from itertools import product

from . import codec, elf, table as tbl

M64 = (1 << 64) - 1
M32 = (1 << 32) - 1


class AsmError(Exception):
    pass


def s64(x):
    x &= M64
    return x - (1 << 64) if x >> 63 else x


# --- lexical helpers ---------------------------------------------------------------------------------------

WORDCH = re.compile(r"[A-Za-z0-9_.$@\\-]")
IDENT = r"[A-Za-z_.$][A-Za-z0-9_.$@]*"
IDENT_RE = re.compile(IDENT)


def strip_comments(text):
    """Remove /* */ comments (they may span lines), `//` and `;` comments, and a `#` at the start of a line.
    String and character literals are left alone."""
    out, i, n = [], 0, len(text)
    in_line_start = True
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
            in_line_start = False
        elif c == "'" and i + 2 < n and (text[i + 2] == "'" or (text[i + 1] == "\\" and i + 3 < n and text[i + 3] == "'")):
            j = i + (4 if text[i + 1] == "\\" else 3)
            out.append(text[i:j])
            i = j
            in_line_start = False
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            if j < 0:
                raise AsmError("unterminated /* comment")
            out.append("\n" * text.count("\n", i, j) if "\n" in text[i:j] else " ")
            i = j + 2
        elif text.startswith("//", i) or c == ";" or (c == "#" and in_line_start):
            j = text.find("\n", i)
            i = n if j < 0 else j
        else:
            out.append(c)
            if c == "\n":
                in_line_start = True
            elif not c.isspace():
                in_line_start = False
            i += 1
    return "".join(out)


def canon(text, info=None):
    """Collapse white space; keep a single space only between two word characters. `info` maps a private-use
    placeholder character to (starts with a word character, ends with one)."""
    def word(c, end):
        if info and c in info:
            return info[c][1 if end else 0]
        return bool(WORDCH.match(c))

    parts = text.split()
    if not parts:
        return ""
    res = parts[0]
    for p in parts[1:]:
        res += (" " if word(res[-1], True) and word(p[0], False) else "") + p
    # A pair's .l/.h names its low/high word (not a 16-bit half).
    def pair_word(m):
        hi, lo = int(m[1]), int(m[2])
        return f"r{lo if m[3] == 'l' else hi}" if hi == lo + 1 and lo % 2 == 0 and hi <= 15 else m[0]
    res = re.sub(r"(?<![\w.])r(\d+)_r(\d+)\.([lh])(?![\w.])", pair_word, res)
    # JieLi's decimal single-precision constants denote their IEEE bit pattern.
    return re.sub(r"(?<![\w.])(-?\d+\.\d*f)(?![\w.])",
                  lambda m: str(struct.unpack("<I", struct.pack("<f", float(m[1][:-1])))[0]), res)


# --- expressions -------------------------------------------------------------------------------------------

TOKEN = re.compile(r"""\s*(?:
    (?P<num>0[xX][0-9a-fA-F]+|0[bB][01]+|[0-9][0-9a-fA-F]*[hH](?![A-Za-z0-9_])|[0-9]+(?![A-Za-z0-9_.$@])
        |[0-9]+[fb](?![A-Za-z0-9_.$@]))
   |(?P<chr>'(?:\\.|[^\\'])'?)
   |(?P<id>[A-Za-z_.$][A-Za-z0-9_.$@]*)
   |(?P<op><<|>>|<=|>=|==|!=|<>|&&|\|\||[-+*/%&|^~!<>()])
)""", re.X)

BIN_LEVEL = {"||": 1, "&&": 2, "==": 3, "!=": 3, "<>": 3, "<": 3, ">": 3, "<=": 3, ">=": 3,
             "+": 4, "-": 4, "|": 5, "&": 5, "^": 5, "!": 5, "*": 6, "/": 6, "%": 6, "<<": 6, ">>": 6}
CHAR_ESC = {"n": 10, "t": 9, "r": 13, "b": 8, "f": 12, "v": 11, "a": 7, "0": 0, "\\": 92, "'": 39, '"': 34}


def tokenize(text):
    pos, out = 0, []
    text = text.rstrip()
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m or m.end() == pos:
            if text[pos:].strip() == "":
                break
            raise AsmError(f"bad token in expression near {text[pos:pos + 12]!r}")
        pos = m.end()
        kind = m.lastgroup
        out.append((kind, m.group(kind)))
    return out


class ExprParser:
    def __init__(self, asm, text, dots=None, operand=False):
        self.asm, self.toks, self.i = asm, tokenize(text), 0
        self.operand = operand      # an instruction operand: register names are not symbols there
        self.text = text
        self.dots = dots            # number of `.` tokens before this expression on its line, or None
        self.seen = 0

    def peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else (None, None)

    def parse(self):
        node = self.binary(1)
        if self.i != len(self.toks):
            raise AsmError(f"junk in expression {self.text!r}")
        return node

    def binary(self, level):
        if level > 6:
            return self.unary()
        left = self.binary(level + 1)
        while True:
            kind, val = self.peek()
            if kind == "op" and BIN_LEVEL.get(val) == level:
                self.i += 1
                left = ("bin", val, left, self.binary(level + 1))
            else:
                return left

    def unary(self):
        kind, val = self.peek()
        if kind == "op" and val in ("-", "+", "~", "!"):
            self.i += 1
            return ("un", val, self.unary())
        return self.primary()

    def primary(self):
        kind, val = self.peek()
        if kind is None:
            raise AsmError(f"expression {self.text!r} ends early")
        self.i += 1
        if kind == "num":
            return self.asm.number(val)
        if kind == "chr":
            body = val[1:].rstrip("'") if len(val) > 1 else ""
            if body.startswith("\\"):
                c = body[1:2]
                if c not in CHAR_ESC:
                    raise AsmError(f"bad character escape {val}")
                return ("num", CHAR_ESC[c])
            return ("num", ord(body[:1]))
        if kind == "id":
            if val == ".":
                self.seen += 1
                key = None if self.dots is None else (self.dots + self.seen - 1)
                return ("sym", self.asm.here(key))
            return self.asm.symref(val, self.operand)
        if val == "(":
            node = self.binary(1)
            if self.peek() != ("op", ")"):
                raise AsmError(f"missing ) in {self.text!r}")
            self.i += 1
            return node
        raise AsmError(f"unexpected {val!r} in expression {self.text!r}")


# @-suffixes after a symbol that the assembler accepts (docs/ELF.md): they only change the relocation type
VARIANTS = {"GOT", "GOTOFF", "GOTPCREL", "GOTTPOFF", "INDNTPOFF", "NTPOFF", "PLT", "TLSGD", "TLSLD", "TLSLDM",
            "TPOFF", "DTPOFF", "PCREL", "PI32V2GOT", "PI32V2GOTFUNCDESC", "PI32V2FUNCDESC"}


DOT = re.compile(r"(?<![A-Za-z0-9_.$@])\.(?![A-Za-z0-9_.$@])")


def walk_syms(node):
    if node[0] in ("sym", "symv"):
        yield node[1]
    elif node[0] == "un":
        yield from walk_syms(node[2])
    elif node[0] == "bin":
        yield from walk_syms(node[2])
        yield from walk_syms(node[3])


# --- operands ----------------------------------------------------------------------------------------------

SR_NUMBER = {name: n for n, name in enumerate(codec.SR_NAMES)}
SR_NUMBER.update({f"sr{n}": n for n in range(16)})
REGISTER_WORDS = set(SR_NUMBER) | {f"r{n}" for n in range(16)}

_MODIMM = {}


def modimm_raw(value, invert=False):
    """A raw 12-bit modified immediate that expands to `value` (the lowest one), or None."""
    if not _MODIMM:
        for raw in range(4096):
            _MODIMM.setdefault(codec.modimm(raw), raw)
    return _MODIMM.get((value ^ M32) & M32 if invert else value & M32)


class Num:
    """A numeric operand whose value is known only at layout time (`neg`: the text was written with the
    sign of an `x - n` alias of an add form)."""

    def __init__(self, node, neg=False, dots=0):
        self.node, self.neg, self.dots = node, neg, dots      # dots: standalone `.` tokens before it on the line


def fit_num(fld, width, v, kind=None, wrap=True):
    """The raw field value for number v in a uimm/simm field, or None if it does not fit. Numbers are 32-bit
    like JieLi's assembler's (`wrap`): 0xFFFFFFFF is -1, 0x100000000 is 0."""
    kind = kind or fld.kind
    v32 = v & M32
    cands = [v]
    if wrap and v32 != v:
        cands.append(v32)
    if wrap and v32 >= 1 << 31:
        cands.append(v32 - (1 << 32))
    for value in cands:
        if fld.asm_range and not fld.asm_range[0] <= value <= fld.asm_range[1]:
            continue
        if fld.zero_as is not None and value == fld.zero_as:
            return 0
        d = value - fld.add
        if d % fld.scale:
            continue
        r = d // fld.scale
        if kind == "uimm":
            if 0 <= r < 1 << width and not (fld.zero_as is not None and r == 0):
                return r
        else:
            if -(1 << (width - 1)) <= r < 1 << (width - 1):
                return r & ((1 << width) - 1)
    return None


def parse_reglist(text):
    """'r3-r0, rets' -> (set of general register numbers, set of other names) or None."""
    regs, names = set(), set()
    text = text.strip()
    if not text:
        return None
    for item in text.split(","):
        item = item.strip().replace(" ", "")
        m = re.fullmatch(r"r(\d+)(?:-r(\d+))?", item)
        if m:
            a = int(m.group(1))
            b = int(m.group(2)) if m.group(2) else a
            if a > 15 or b > 15:
                return None
            regs.update(range(min(a, b), max(a, b) + 1))
        elif re.fullmatch(IDENT, item):
            names.add(item)
        else:
            return None
    return regs, names


def operand_raw(insn, name, text):
    """Convert operand text to a raw field value (int), a Num marker for expressions (handled by the caller),
    or None when the text is not valid for the field."""
    fld = insn.fields[name]
    kind, width = fld.kind, len(insn.field_bits(name))
    if kind in ("gpr", "gpr_lo", "gpr_hi"):
        m = re.fullmatch(r"r(\d+)", text)
        if not m or len(text) > 3 or (len(m.group(1)) > 1 and m.group(1)[0] == "0"):
            return None
        n = int(m.group(1))
        if n > 15:
            return None
        if kind == "gpr_lo":
            return n if n < 8 else None
        if kind == "gpr_hi":
            return n - 8 if n >= 8 else None
        return n
    if kind == "pair":
        m = re.fullmatch(r"r(\d+)_r(\d+)", text)
        if not m:
            return None
        hi, lo = int(m.group(1)), int(m.group(2))
        return lo // 2 if hi == lo + 1 and lo % 2 == 0 and hi <= 15 else None
    if kind == "pair_regs":
        m = re.fullmatch(r"r(\d+),r(\d+)", text.replace(" ", ""))
        if not m:
            return None
        a, b = int(m.group(1)), int(m.group(2))
        if a > 15 or b > 15 or a // 2 != b // 2:
            return None
        return (a // 2) << 2 | (a & 1) << 1 | (b & 1)
    if kind == "sr":
        return SR_NUMBER.get(text)
    if kind == "enum":
        want = canon(text)
        for raw, v in enumerate(fld.values):
            if canon(v) == want:
                return raw
        return None
    if kind in ("rlist4", "rmask", "srmask"):
        got = parse_reglist(text)
        if got is None:
            return None
        regs, names = got
        if kind == "rlist4":
            if names or not regs:
                return None
            lo, hi = min(regs), max(regs)
            if regs != set(range(lo, hi + 1)):
                return None
            if hi == 3 and lo <= 3:
                return lo
            return hi if lo == 4 and hi >= 4 else None
        if kind == "rmask":
            want = {fld.lead} if fld.lead else set()
            if names != want:
                return None
            return sum(1 << r for r in regs)
        if regs:
            return None
        mask = 0
        for nm in names:
            if nm not in SR_NUMBER or SR_NUMBER[nm] >= width:
                return None
            mask |= 1 << SR_NUMBER[nm]
        return mask
    raise AssertionError(kind)


NUMERIC_KINDS = ("uimm", "simm", "bit", "invbit", "modimm")


def numeric_raw(insn, name, value):
    """Raw field value for a constant number in a numeric-kind field, or None."""
    fld = insn.fields[name]
    kind, width = fld.kind, len(insn.field_bits(name))
    if kind in ("uimm", "simm"):
        return fit_num(fld, width, value)
    if kind in ("bit", "invbit"):
        v = value & M32
        if kind == "invbit":
            v ^= M32
        return v.bit_length() - 1 if v and v & (v - 1) == 0 else None
    if kind == "modimm":
        return modimm_raw(value, fld.invert)
    raise AssertionError(kind)


# --- matchers ----------------------------------------------------------------------------------------------

OPS = ["<<<", ">>>", "<<", ">>", "+", "-", "*", "&", "|", "^"]
SENT = 0xE000


def _template(insn, syntax):
    """Canonical text of a syntax with a private-use sentinel per placeholder, and the placeholder list."""
    holders, out, info = [], [], {}
    pos = 0
    for m in tbl.SYNTAX_TOKEN.finditer(syntax):
        out.append(syntax[pos:m.start()])
        if m.group(0) in ("{{", "}}"):
            out.append(m.group(0)[0])
        else:
            holders.append((m.group(2), bool(m.group(1))))
            ch = chr(SENT + len(holders) - 1)
            fld = insn.fields[m.group(2)]
            if fld.kind == "enum":
                vals = [canon(v) for v in fld.values if v]
                info[ch] = (any(WORDCH.match(v[0]) for v in vals), any(WORDCH.match(v[-1]) for v in vals))
            out.append(ch)
        pos = m.end()
    out.append(syntax[pos:])
    return canon("".join(out), info), holders


def _kind_regex(insn, name, group):
    fld = insn.fields[name]
    k = fld.kind
    if k in ("gpr", "gpr_lo", "gpr_hi"):
        return rf"(?P<{group}>r\d+)"
    if k == "pair":
        return rf"(?P<{group}>r\d+_r\d+)"
    if k == "pair_regs":
        return rf"(?P<{group}>r\d+,r\d+)"
    if k == "sr":
        return rf"(?P<{group}>[A-Za-z_][A-Za-z0-9_]*)"
    if k == "enum":
        alts = sorted({canon(v) for v in fld.values}, key=len, reverse=True)
        return rf"(?P<{group}>" + "|".join(re.escape(a) for a in alts) + ")"
    if k in ("rlist4", "rmask", "srmask"):
        return rf"(?P<{group}>[^{{}}]*?)"
    return rf"(?P<{group}>.+?)"


class Matcher:
    """One way to write an entry: a compiled regular expression and how its groups map to fields."""

    def __init__(self, insn, order, syntax, alias=None, u_optional=False):
        self.insn, self.order, self.alias = insn, order, alias
        self.negate = alias[1] if alias and alias[0] == "neg" else None
        self.rejects = {canon(t) for t in insn.asm_rejects}
        text, holders = _template(insn, syntax)
        self.holders = holders
        seen, parts, groups = {}, [], {}
        skip_space = False
        for pos, ch in enumerate(text):
            code = ord(ch)
            if ch == " " and skip_space:
                skip_space = False
                continue
            if SENT <= code < SENT + 0x100:
                name, plus = holders[code - SENT]
                if name in seen:
                    parts.append(rf"(?P={seen[name]})")
                    continue
                group = f"f_{name}"
                seen[name] = group
                body = _kind_regex(insn, name, group)
                if insn.fields[name].kind in NUMERIC_KINDS:
                    # a space in the template next to a number: optional when the number starts or ends
                    # with a character that is not part of a word (`goto -2`, `rep (4) r1`)
                    if pos and text[pos - 1] == " " and not plus:
                        parts[-1] = r"(?: |(?![A-Za-z0-9_.$@]))"
                    if pos + 1 < len(text) and text[pos + 1] == " ":
                        skip_space = True
                        body = body + r"(?: |(?<![A-Za-z0-9_.$@]))"
                parts.append(rf"(?:\+{body})?" if plus else body)
                groups[name] = group
            else:
                parts.append(re.escape(ch))
        pattern = "".join(parts)
        if u_optional:
            pattern = re.sub(r"\\\(u\\\)((?:\\#)?)$", r"(?:\\(u\\))?\1", pattern)
        self.template = text
        self.regex = re.compile(pattern + "$", re.S)
        self.groups = groups
        self.plus = {n for n, p in holders if p}


def alias_syntaxes(insn, syntax):
    """Other ways to write an entry, as (syntax, negated numeric field or None): `{d} += {s}` is also
    `{d} = {d} + {s}` (and `{d} = {s} + {d}` for the commutative operations); `{r} += {n}` and
    `{d} = {s} + {n}` are also written with `-` and the opposite number."""
    out = []
    m = re.fullmatch(r"\{(\w)\} (<<<|>>>|<<|>>|\+|-|\*|&|\||\^)= (.+)", syntax)
    if m:
        d, op, rest = m.groups()
        out.append((f"{{{d}}} = {{{d}}} {op} {rest}", None))
        if op in "+*&|^" and rest == "{" + rest[1:-1] + "}" and rest[1:-1] in insn.fields \
                and insn.fields[rest[1:-1]].kind in ("gpr", "gpr_lo", "gpr_hi"):
            out.append((f"{{{d}}} = {rest} {op} {{{d}}}", None))
    m = re.fullmatch(r"\{(\w)\} = \{(\w)\} (<<<|>>>|<<|>>|\+|-|\*|&|\||\^) \{(\w)\}(\s+#)?", syntax)
    if m and insn.fields[m.group(1)].kind == insn.fields[m.group(2)].kind:
        d, a, op, b, par = m.groups()
        out.append((f"{{{d}}} {op}= {{{b}}}{par or ''}", ("same", a, d)))
    m = re.fullmatch(r"(.*) \+(=?) \{(\w)\}", syntax)
    if m and insn.fields[m.group(3)].kind in NUMERIC_KINDS:
        out.append((f"{m.group(1)} -{m.group(2)} {{{m.group(3)}}}", ("neg", m.group(3))))
        m2 = re.fullmatch(r"\{(\w)\} \+= \{(\w)\}", syntax)
        if m2:
            out.append((f"{{{m2.group(1)}}} = {{{m2.group(1)}}} - {{{m2.group(2)}}}", ("neg", m2.group(2))))
    # Operand-less spellings supply fixed raw fields. Keep the encoding in
    # the table, including the increment's scale and the modified immediate.
    base_id = insn.id.removesuffix(".p")
    par = " #" if insn.id.endswith(".p") else ""
    if re.match(r"if \((?:--)?\{\w\} (?:==|!=) ", syntax):
        out.append((syntax.replace("if ", "ifs ", 1), None))
    if base_id in ("xchg.rrr", "xchg.rrr.sat"):
        suffix = " (sat)" if base_id.endswith(".sat") else ""
        out.append(("{r} ><= {t}" + suffix + par, ("same", "s", "r")))
    if base_id in ("lsr.s.rpi", "lsr.s.rpr"):
        out.append((syntax.replace(">>", ">>>").replace(" (s)", ""), None))
    if "rnd" in syntax and "uavg" in syntax:
        out.append((re.sub(r"\(rnd,\s*uavg\)", "(uavg,rnd)", syntax), None))
    if base_id == "xor.modimm.rri":
        out.append(("{r} = -{s} (f)" + par, ("fixed", {"n": numeric_raw(insn, "n", 0x80000000)})))
        # `rD = ~rS` on a register the 2-byte not cannot name (JieLi's assembler: an xor with all ones)
        out.append(("{r} = ~{s}" + par, ("fixed", {"n": numeric_raw(insn, "n", 0xFFFFFFFF)})))
    if base_id.startswith("cmp.") and "(v)" in syntax:
        out.append((syntax.replace("cmp(", "cmpu("), None))
    if base_id == "rsub.modimm.rri":
        out.append(("{r} = -{s}" + par, ("fixed", {"n": numeric_raw(insn, "n", 0)})))
    if base_id == "and.nmodimm.rri":
        out.append(("{r} = fabs({s})" + par, ("fixed", {"n": numeric_raw(insn, "n", 0x7FFFFFFF)})))
    # Each memory operand may independently omit its natural increment.
    # The six mem2.hh immediate forms reject implicit steps, even though
    # their disassembler syntax accepts explicit halfword-sized offsets.
    if ".mem2.hh.i." in base_id:
        return out
    mem = list(re.finditer(r"([bhd]?)\[\{(\w)\}\s*\+\+=\s*\{(\w)\}\]", syntax))
    choices = []
    for m in mem:
        name = m[3]
        options = [(m[0], {})]
        if insn.fields[name].kind in NUMERIC_KINDS:
            step = {"": 4, "h": 2, "b": 1, "d": 8}[m[1]]
            for sign, op in ((1, "++"), (-1, "--")):
                raw = numeric_raw(insn, name, sign * step)
                if raw is not None:
                    options.append((f"{m[1]}[{{{m[2]}}}{op}]", {name: raw}))
        choices.append(options)
    for combo in product(*choices):
        fixed = {k: v for _, fields in combo for k, v in fields.items()}
        if not fixed:
            continue
        sx, pos = "", 0
        for m, (text, _) in zip(mem, combo):
            sx += syntax[pos:m.start()] + text
            pos = m.end()
        out.append((sx + syntax[pos:], ("fixed", fixed)))
    return out


class Candidate:
    def __init__(self, insn, ops, order, alias=False):
        self.insn, self.ops, self.order, self.alias = insn, ops, order, alias
        self.size = 2 * len(insn.words)


def branch_field(insn):
    """The field holding a pc-relative target (`goto {n}`, `call {n}`, `gotoss {n}`), or None."""
    m = re.search(r"\b(?:goto|call|gotoss) \{(\w)\}(?: \(\w\))?$", insn.syntax)
    if m and insn.fields[m.group(1)].kind == "simm":
        return m.group(1)
    return None


def abs32_field(insn):
    """The field of a 32-bit constant load, which can carry a relocation, or None."""
    for name, f in insn.fields.items():
        if f.kind == "uimm" and len(insn.field_bits(name)) == 32:
            return name
    return None


BRANCH_RELOC = {(2, 16): 1, (2, 22): 2, (3, 16): 3, (3, 31): 4}


class Isa:
    """The matchers built from a table."""

    def __init__(self, tab):
        self.table = tab
        self.insns = list(tab.insns)
        self.matchers = []
        self.by_key = {}
        self.cache = {}
        self.branch = {i.id: branch_field(i) for i in self.insns}
        self.by_id = {i.id: i for i in self.insns}
        self.ifblock = {i.id: bool(i.block) for i in self.insns}
        self.abs32 = {i.id: abs32_field(i) for i in self.insns}
        position = {i.id: n for n, i in enumerate(self.insns)}
        for insn in self.insns:
            if insn.asm_no_syntax:      # objdump prints it, but no assembler syntax produces it (JieLi's refuses it)
                continue
            order = position.get(insn.id.removesuffix(".p"), position[insn.id])    # a twin ranks as its base
            main = insn.asm_syntax or insn.syntax
            u_opt = insn.id.removesuffix(".p") not in U_REQUIRED
            self.matchers.append(Matcher(insn, order, main, u_optional=u_opt))
            for sx, alias in alias_syntaxes(insn, main):
                self.matchers.append(Matcher(insn, order, sx, alias=alias, u_optional=u_opt))
        # index by the literal prefix before the first placeholder
        for m in self.matchers:
            self.by_key.setdefault(self._prefix(m.template), []).append(m)
        self.prefix_lengths = sorted({len(k) for k in self.by_key}, reverse=True)

    @staticmethod
    def _prefix(text):
        out = []
        for ch in text:
            if SENT <= ord(ch) < SENT + 0x100:
                break
            out.append(ch)
        return "".join(out).rstrip(" ")

    def candidates(self, line):
        """All (insn, operands) readings of a canonical instruction line; operand values are raw ints or Num."""
        out = []
        plain = re.sub(r"(?<![A-Za-z0-9_.$@])(?:0[xX][0-9a-fA-F]+|0[bB][01]+|0[0-7]+)(?![A-Za-z0-9_.$@])",
                       lambda m: str(int(m.group(0), 0) if m.group(0)[:2].lower() in ("0x", "0b") else int(m.group(0), 8)),
                       line)
        for text in dict.fromkeys((line, plain)):
            for key_len in self.prefix_lengths:
                for m in self.by_key.get(text[:key_len], ()):
                    got = m.regex.match(text)
                    if got and text not in m.rejects and not any(c.matcher is m for c in out):
                        ops = self._operands(m, got)
                        if ops is not None:
                            c = Candidate(m.insn, ops, m.order, bool(m.negate))
                            c.matcher = m
                            out.append(c)
        return out

    def _operands(self, m, got):
        insn, ops = m.insn, {}
        for name, group in m.groups.items():
            text = got.group(group)
            if text is None:
                if name in m.plus:
                    ops[name] = 0
                    continue
                return None
            fld = insn.fields[name]
            if fld.kind in NUMERIC_KINDS:
                ops[name] = Num(text, name == m.negate, len(DOT.findall(got.string[:got.start(group)])))
            else:
                raw = operand_raw(insn, name, text)
                if raw is None:
                    return None
                ops[name] = raw
        if m.alias and m.alias[0] == "same":
            ops[m.alias[1]] = ops[m.alias[2]]
        if m.alias and m.alias[0] == "fixed":
            ops.update(m.alias[1])
        return ops


# --- assembler state ---------------------------------------------------------------------------------------

class Sym:
    def __init__(self, name):
        self.name = name
        self.kind = "undef"          # undef | label | abs | alias
        self.section = None
        self.item = 0
        self.offset = 0
        self.value = 0
        self.node = None             # alias: the expression
        self.bind = elf.STB_LOCAL
        self.declared = False        # .globl / .weak seen
        self.local_decl = False      # .local seen
        self.type = elf.STT_NOTYPE
        self.other = 0
        self.size_node = None
        self.common = None           # (size, align)
        self.pure = False            # abs: a constant that involved no labels and no forward references
        self.reloc_ref = False       # used by a relocation: must be in the symbol table
        self.used = False
        self.temp = name.startswith(".L")


class Sec:
    def __init__(self, name, type_, flags, align=1, entsize=0):
        self.name, self.type, self.flags, self.align, self.entsize = name, type_, flags, align, entsize
        self.items = []
        self.offs = []
        self.size = 0
        self.group = None


class Item:
    """An item of a section; `size(asm)` is its size at the current layout."""
    fixed = True
    loc = None


class Raw(Item):
    def __init__(self, data):
        self.data = bytes(data)

    def size(self, asm, sec, off):
        return len(self.data)

    def emit(self, asm, sec, off, out):
        out.data += self.data


class Data(Item):
    """.byte/.short/.long/.quad with an expression."""

    def __init__(self, width, node, loc):
        self.width, self.node, self.loc = width, node, loc

    def size(self, asm, sec, off):
        return self.width

    def emit(self, asm, sec, off, out):
        asm.variant = None
        sym, add = asm.ev(self.node, self.loc)
        if sym is None:
            lo, hi = -(1 << (8 * self.width - 1)), (1 << (8 * self.width)) - 1
            if self.width < 8 and not lo <= add <= hi:
                raise AsmError(f"{self.loc}: out of range literal value in {self.width}-byte data")
            out.data += (add & ((1 << (8 * self.width)) - 1)).to_bytes(self.width, "little")
            return
        types = {1: 6, 2: 7, 4: 8}
        if self.width not in types:
            raise AsmError(f"{self.loc}: a {self.width}-byte relocation is not supported")
        sym = asm.reloc_symbol(sym)
        rtype = 11 if asm.variant and self.width == 4 else types[self.width]
        out.relocs.append((off, rtype, sym.name, add))
        out.data += bytes(self.width)


class Space(Item):
    """.space/.fill/.zero: `count` units of `unit` bytes filled with `value`."""

    def __init__(self, count, unit, value, loc, strict=False, fill=False):
        self.count, self.unit, self.value, self.loc, self.strict = count, unit, value, loc, strict
        self.fill = fill                # .fill: in a NOBITS section its value must be 0 (.space's is ignored there)
        self.cache = 0

    def size(self, asm, sec, off):
        sym, n = asm.ev(self.count, self.loc, provisional=True)
        if sym is not None:
            raise AsmError(f"{self.loc}: size must be absolute")
        if n < 0 or (n == 0 and self.strict):
            raise AsmError(f"{self.loc}: invalid number of bytes")
        self.cache = n * self.unit
        return self.cache

    def emit(self, asm, sec, off, out):
        sym, v = asm.ev(self.value, self.loc) if self.value is not None else (None, 0)
        n = self.cache
        if self.unit == 1 or self.value is None:
            out.data += bytes([v & 0xFF]) * n
        else:
            out.data += (v & ((1 << (8 * min(self.unit, 8))) - 1)).to_bytes(min(self.unit, 8), "little") \
                .ljust(self.unit, b"\0") * (n // self.unit)


class Align(Item):
    def __init__(self, boundary, fill, maxskip, loc):
        self.boundary, self.fill, self.maxskip, self.loc = boundary, fill, maxskip, loc
        self.pad = 0

    def size(self, asm, sec, off):
        b = self.boundary
        pad = (-off) % b
        if self.maxskip is not None and pad > self.maxskip:
            pad = 0
        self.pad = pad
        return pad

    def emit(self, asm, sec, off, out):
        out.data += bytes([self.fill]) * self.pad


class Org(Item):
    def __init__(self, node, fill, loc):
        self.node, self.fill, self.loc = node, fill, loc
        self.pad = 0

    def size(self, asm, sec, off):
        sym, n = asm.ev(self.node, self.loc, provisional=True)
        if sym is not None:
            if sym.section is not sec:
                raise AsmError(f"{self.loc}: .org target must be in the current section")
            n += asm.sym_offset(sym)
        if n < off:
            raise AsmError(f"{self.loc}: .org moves backwards")
        self.pad = n - off
        return self.pad

    def emit(self, asm, sec, off, out):
        out.data += bytes([self.fill]) * self.pad


class Leb(Item):
    def __init__(self, node, signed, loc):
        self.node, self.signed, self.loc = node, signed, loc
        self.data = b""

    def size(self, asm, sec, off):
        sym, v = asm.ev(self.node, self.loc, provisional=True)
        if sym is not None:
            raise AsmError(f"{self.loc}: LEB128 operand must be absolute")
        out = bytearray()
        while True:
            b = v & 0x7F
            v >>= 7
            if self.signed:
                done = (v == 0 and not b & 0x40) or (v == -1 and b & 0x40)
            else:
                done = v == 0
            out.append(b | (0 if done else 0x80))
            if done:
                break
        self.data = bytes(out)
        return len(self.data)

    def emit(self, asm, sec, off, out):
        out.data += self.data


class Insn(Item):
    fixed = False

    def __init__(self, cands, loc, fixed_const, body=None):
        self.cands = cands        # sorted: preferred first within a size
        self.loc = loc
        self.fixed_const = fixed_const
        self.body = body          # (start label, end label) for `rep`
        self.chosen = None
        self.error = None
        self.cur = 0              # current size
        self.sec = None
        self.off = 0

    def size(self, asm, sec, off):
        self.sec, self.off = sec, off
        return self.cur

    def emit(self, asm, sec, off, out):
        self.sec, self.off = sec, off
        data, reloc = asm.encode_cand(self, self.chosen, off)
        if reloc:
            out.relocs.append((off, reloc[0], reloc[1].name, reloc[2]))
        out.data += data


class OutSec:
    def __init__(self):
        self.data = bytearray()
        self.relocs = []
        self.size = 0


# --- the assembler -----------------------------------------------------------------------------------------

AX, WA = elf.SHF_ALLOC | elf.SHF_EXECINSTR, elf.SHF_ALLOC | elf.SHF_WRITE
# What `.section NAME` (no flags) gives for the names JieLi's assembler knows: (type, flags, entry size). Other
# names get PROGBITS and no flags (`.note*` gets SHT_NOTE).
KNOWN_SECTIONS = {
    ".text": (elf.SHT_PROGBITS, AX, 0), ".data": (elf.SHT_PROGBITS, WA, 0), ".bss": (elf.SHT_NOBITS, WA, 0),
    ".rodata": (elf.SHT_PROGBITS, elf.SHF_ALLOC, 0), ".init": (elf.SHT_PROGBITS, AX, 0),
    ".fini": (elf.SHT_PROGBITS, AX, 0), ".tbss": (elf.SHT_NOBITS, WA | elf.SHF_TLS, 0),
    ".tdata": (elf.SHT_PROGBITS, WA | elf.SHF_TLS, 0), ".data.rel.ro": (elf.SHT_PROGBITS, WA, 0),
    ".eh_frame": (elf.SHT_PROGBITS, WA, 0), ".gcc_except_table": (elf.SHT_PROGBITS, elf.SHF_ALLOC, 0),
    ".init_array": (14, 0, 0), ".fini_array": (15, 0, 0), ".preinit_array": (16, 0, 0),
    ".debug_str": (elf.SHT_PROGBITS, elf.SHF_MERGE | elf.SHF_STRINGS, 1),
}
for _n in (4, 8, 16, 32):
    KNOWN_SECTIONS[f".rodata.cst{_n}"] = (elf.SHT_PROGBITS, elf.SHF_ALLOC | elf.SHF_MERGE, _n)


def default_section(name):
    if name in KNOWN_SECTIONS:
        return KNOWN_SECTIONS[name]
    return (7 if name.startswith(".note") else elf.SHT_PROGBITS), 0, 0


SECTION_TYPES = {"progbits": elf.SHT_PROGBITS, "nobits": elf.SHT_NOBITS, "note": 7, "init_array": 14,
                 "fini_array": 15, "preinit_array": 16}
SECTION_FLAGS = {"a": elf.SHF_ALLOC, "w": elf.SHF_WRITE, "x": elf.SHF_EXECINSTR, "M": elf.SHF_MERGE,
                 "S": elf.SHF_STRINGS, "T": elf.SHF_TLS}
SYMBOL_TYPES = {"function": elf.STT_FUNC, "object": elf.STT_OBJECT, "notype": elf.STT_NOTYPE,
                "tls_object": elf.STT_TLS, "common": elf.STT_COMMON, "STT_FUNC": elf.STT_FUNC,
                "STT_OBJECT": elf.STT_OBJECT, "STT_NOTYPE": elf.STT_NOTYPE, "STT_TLS": elf.STT_TLS,
                "gnu_indirect_function": 10}

STRING_ESC = {"n": 10, "t": 9, "r": 13, "b": 8, "f": 12, "\\": 92, '"': 34, "a": 7, "v": 11}


class Block:
    def __init__(self, kind, insn_item, end_label):
        self.kind, self.item, self.end_label = kind, insn_item, end_label
        self.counts = [0, 0]        # instructions in the then and else parts (`if` blocks)
        self.in_else = False


class _Core:
    def __init__(self, tab=None, include=(), features=None, chip=None, chip_errors=False):
        self.features = None if features is None else set(features)     # None: every entry is allowed
        self.chip, self.chip_errors = chip, chip_errors     # warn (or refuse) forms the table's hw marks on chip
        self.hw_warnings = {}                               # message -> None (ordered, once per line and form)
        self.table = tab or _default_table()
        self.isa = _isa_for(self.table)
        self.include = [str(p) for p in include]
        self.sections = {}
        self.order = []
        self.syms = {}
        self.cur = None
        self.stack = []
        self.prev = None
        self.file_names = []
        self.local = {}              # numeric label -> {"def": last definition, "pending": forward reference}
        self.tmp_id = 0              # .Ltmp<n> temporaries (anonymous labels)
        self.stmt_serial = 0
        self.dot_cache = {}
        self.counter = 0             # names for the labels of block bodies
        self.macro_count = 0         # \@: macro invocations so far
        self.macros = {}
        self.blocks = []
        self.pending_pair = False
        self.cond = []               # conditional assembly stack: (active, taken, parent_active)
        self.loc = "<input>"
        self.switch(".text")
        self.warnings = []
        self._prov = False
        self.variant = None

    # -- sections ------------------------------------------------------------------------------------------
    def switch(self, name, type_=None, flags=None, entsize=0):
        sec = self.sections.get(name)
        if sec is None:
            if type_ is None:
                type_, flags, entsize = default_section(name)
            sec = Sec(name, type_, flags or 0, 2 if name == ".text" else 1, entsize)
            self.sections[name] = sec
            self.order.append(sec)
        if self.cur is not sec:
            self.prev = self.cur
        self.cur = sec
        return sec

    # -- symbols -------------------------------------------------------------------------------------------
    def sym(self, name):
        s = self.syms.get(name)
        if s is None:
            s = self.syms[name] = Sym(name)
        return s

    def number(self, text):
        t = text.lower()
        if re.fullmatch(r"[0-9]+[fb]", t):                     # 1f, 1b: the next / previous numeric label 1
            return self.local_ref(t[:-1], t[-1])
        if t.startswith("0x"):
            return ("num", int(t, 16))
        if t.startswith("0b") and re.fullmatch(r"0b[01]+", t):
            return ("num", int(t[2:], 2))
        if t.endswith("h"):
            return ("num", int(t[:-1], 16))
        if len(t) > 1 and t[0] == "0":
            try:
                return ("num", int(t, 8))
            except ValueError:
                raise AsmError(f"bad octal number {text}") from None
        return ("num", int(t))

    def local_ref(self, n, direction):
        slot = self.local.setdefault(n, {"def": None, "pending": None})
        if direction == "b":
            if slot["def"] is None:
                raise AsmError(f"{self.loc}: undefined backward reference {n}b")
            s = slot["def"]
        else:
            if slot["pending"] is None:
                slot["pending"] = self.new_tmp()
            s = slot["pending"]
        s.used = True
        return ("sym", s)

    def symref(self, name, operand=False):
        if name == ".":
            return ("sym", self.here())
        if "@" in name:
            name, _, var = name.partition("@")
            if var.upper() not in VARIANTS:
                raise AsmError(f"{self.loc}: invalid variant '{var}'")
            return ("symv", self.symref(name)[1], var.upper())
        # JieLi's assembler takes a register-named symbol (a C function `pc`) in directives and data, but
        # not as an instruction operand
        if operand and (name in REGISTER_WORDS or re.fullmatch(r"(?:r\d+(?:_r\d+)?|sr\d+)(?:\.\w+)?", name)):
            raise AsmError(f"register name {name} in an expression")
        s = self.sym(name)
        s.used = True
        return ("sym", s)

    def new_tmp(self):
        """A temporary symbol named like LLVM's: .Ltmp0, .Ltmp1, ..."""
        s = Sym(f".Ltmp{self.tmp_id}")
        self.tmp_id += 1
        s.temp = True
        self.syms[s.name] = s
        return s

    def here(self, key=None):
        """An anonymous label at the current location. `key` (the index of this `.` among those on the
        statement's line) makes the several readings of one instruction share one label."""
        if key is not None and (self.stmt_serial, key) in self.dot_cache:
            return self.dot_cache[(self.stmt_serial, key)]
        s = self.new_tmp()
        self.define_label(s)
        if key is not None:
            self.dot_cache[(self.stmt_serial, key)] = s
        return s

    def define_label(self, s):
        if s.kind != "undef":
            raise AsmError(f"{self.loc}: symbol {s.name} is already defined")
        s.kind = "label"
        s.section = self.cur
        s.item = len(self.cur.items)
        return s

    def sym_pos(self, s):
        """(section, offset) of a label or alias, or None."""
        if s.kind == "label":
            return s.section, s.offset
        if s.kind == "alias":
            r = self.ev(s.node, None)
            if r[0] is not None and r[0] is not s:
                p = self.sym_pos(r[0])
                if p:
                    return p[0], p[1] + r[1]
        return None

    def alias_base(self, s):
        """The symbol an alias (`.set A, B+4`) ends up naming, following chains of aliases."""
        for _ in range(100):
            if s.kind != "alias":
                return s
            nxt = self.ev(s.node, None)[0]
            if nxt is None or nxt is s:
                return None
            s = nxt
        return None

    def sym_offset(self, s):
        p = self.sym_pos(s)
        if p is None:
            raise AsmError(f"{self.loc}: symbol {s.name} is not defined in a section")
        return p[1]

    # -- expression evaluation -----------------------------------------------------------------------------
    def ev(self, node, loc, provisional=False):
        """(symbol or None, addend). A symbol result is relocatable: a label, alias or undefined name."""
        t = node[0]
        if t == "num":
            return None, node[1]
        if t == "symv":
            r = self.ev(("sym", node[1]), loc, provisional)
            if r[0] is None:
                return r
            self.variant = node[2]
            return r
        if t == "sym":
            s = node[1]
            if s.kind == "abs":
                return None, s.value
            if s.kind == "alias":
                r = self.ev(s.node, loc, provisional)
                return (None, r[1]) if r[0] is None else (s, 0)
            if provisional and s.kind == "label":
                self.provisional_offsets(s.section)
            return s, 0
        if t == "un":
            sym, v = self.ev(node[2], loc, provisional)
            if sym is not None:
                raise AsmError(f"{loc}: expected relocatable expression")
            op = node[1]
            return None, s64({"-": -v, "+": v, "~": ~v, "!": int(v == 0)}[op])
        _, op, a, b = node
        sa, va = self.ev(a, loc, provisional)
        sb, vb = self.ev(b, loc, provisional)
        if op in ("+", "-"):
            if sa is None and sb is None:
                return None, s64(va + vb if op == "+" else va - vb)
            if sb is None:
                return sa, s64(va + vb if op == "+" else va - vb)
            if op == "+" and sa is None:
                return sb, s64(va + vb)
            if op == "-" and sa is not None:
                pa, pb = self.sym_pos(sa), self.sym_pos(sb)
                if pa and pb and pa[0] is pb[0]:
                    if provisional:
                        self.provisional_offsets(pa[0])
                    return None, s64(self.sym_offset(sa) + va - self.sym_offset(sb) - vb)
            raise AsmError(f"{loc}: can't resolve this expression (symbol difference across sections or undefined)")
        if sa is not None or sb is not None:
            raise AsmError(f"{loc}: expected relocatable expression")
        if op in ("/", "%"):
            if vb == 0:
                raise AsmError(f"{loc}: division by zero")
            q = abs(va) // abs(vb) * (1 if (va < 0) == (vb < 0) else -1)
            return None, s64(q if op == "/" else va - q * vb)
        r = {
            "*": lambda: va * vb, "<<": lambda: (va << (vb & 63)), ">>": lambda: ((va & M64) >> (vb & 63)),
            "&": lambda: va & vb, "|": lambda: va | vb, "^": lambda: va ^ vb, "!": lambda: va | ~vb,
            "==": lambda: int(va == vb), "!=": lambda: int(va != vb), "<>": lambda: int(va != vb),
            "<": lambda: int(va < vb), ">": lambda: int(va > vb), "<=": lambda: int(va <= vb),
            ">=": lambda: int(va >= vb), "&&": lambda: int(bool(va) and bool(vb)),
            "||": lambda: int(bool(va) or bool(vb)),
        }[op]()
        return None, s64(r)

    def provisional_offsets(self, sec):
        """Offsets of the labels of `sec` from the items' current sizes (used while parsing)."""
        if self._prov:
            return
        self._prov = True
        try:
            off = 0
            offs = []
            for item in sec.items:
                offs.append(off)
                if isinstance(item, Insn):
                    off += item.cur
                elif isinstance(item, Align):
                    off += (-off) % item.boundary
                else:
                    try:
                        off += item.size(self, sec, off)
                    except AsmError:
                        pass
            offs.append(off)
            for s in self.syms.values():
                if s.kind == "label" and s.section is sec:
                    s.offset = offs[min(s.item, len(offs) - 1)]
        finally:
            self._prov = False

    def parse_expr(self, text, dots=None, operand=False):
        text = text.strip()
        if not text:
            raise AsmError(f"{self.loc}: missing expression")
        node = ExprParser(self, text, dots, operand).parse()
        if node[0] != "num" and self.is_pure(node):         # a constant keeps the value it has now (.set may change it)
            return ("num", self.ev(node, self.loc)[1])
        return node

    def const_expr(self, text, what="expression"):
        """Evaluate an expression that must be a constant right now."""
        node = self.parse_expr(text)
        sym, v = self.ev(node, self.loc, provisional=True)
        if sym is not None:
            raise AsmError(f"{self.loc}: {what} must be absolute")
        return v

    def is_pure(self, node):
        """True if an expression is a constant that involved no labels and no forward references."""
        for s in walk_syms(node):
            if s.kind != "abs" or not s.pure:
                return False
        return True

    # -- emission of instructions --------------------------------------------------------------------------
    def encode_cand(self, item, cand, off):
        """(bytes, reloc or None) for candidate `cand` of `item` at section offset `off`, or None if it cannot
        be encoded with the current layout. A reloc is (type, symbol, addend)."""
        insn = cand.insn
        isa = self.isa
        values, reloc = {}, None
        sec = item.sec
        for name, op in cand.ops.items():
            if not isinstance(op, Num):
                values[name] = op
                continue
            try:
                self.variant = None
                sym, add = self.ev(op.node, item.loc)
            except AsmError as e:
                item.error = item.error or e
                return None
            fld = insn.fields[name]
            width = len(insn.field_bits(name))
            if name == isa.branch[insn.id]:
                end = off + cand.size
                if sym is None:
                    disp = add
                else:
                    pos = self.sym_pos(sym)
                    if pos and pos[0] is sec and sym.bind != elf.STB_WEAK and sym.kind != "undef":
                        disp = pos[1] + add - end
                    else:
                        rtype = BRANCH_RELOC.get((len(insn.words), width))
                        if rtype is None or (insn.id == "call.xl"):
                            return None
                        reloc = (rtype, self.reloc_symbol(sym), add)
                        values[name] = 0
                        continue
                if disp % 2:
                    raise AsmError(f"{item.loc}: branch displacement {disp} is odd")
                raw = fit_num(fld, width, disp, "simm", wrap=False)
                if raw is None:
                    return None
                values[name] = raw
                continue
            if sym is not None:
                if name == isa.abs32[insn.id]:
                    reloc = ({"PI32V2GOT": 9, "PI32V2GOTFUNCDESC": 10}.get(self.variant, 5), self.reloc_symbol(sym), add)
                    values[name] = 0
                    continue
                return None
            if name == isa.abs32[insn.id] and not item.fixed_const:
                values[name] = add & M32
                continue
            # A subtraction alias must not wrap its negated constant into a
            # small addition: r3 = r0 - 0xFFFFFFFF keeps the subtract form.
            if cand.alias and cand.size == 2 and not -(1 << 31) <= add < (1 << 31):
                return None
            raw = numeric_raw(insn, name, add)
            if raw is None:
                return None
            values[name] = raw
        if insn.body:
            n = self.body_length(item)
            raw = fit_num(insn.fields[insn.body], len(insn.field_bits(insn.body)), n)
            if raw is None:
                return None
            values[insn.body] = raw
        if insn.block:                  # an if-block opener: its shape fields come from the block's body
            blk = getattr(item, "block", None)
            then, els = blk.counts if blk is not None and blk.kind == "if" else (1, 0)
            for part, n in (("then", max(then, 1)), ("else", els)):
                name = insn.block.get(part)
                if name:
                    raw = fit_num(insn.fields[name], len(insn.field_bits(name)), n)
                    if raw is None:
                        return None
                    values[name] = raw
        note = self.chip and tbl.hw_note(insn, self.chip)
        if note:
            msg = f"{item.loc}: {insn.id} {note} on the {self.chip} (isa/pi32v2.toml, hw)"
            if self.chip_errors:
                raise AsmError(msg)
            self.hw_warnings[msg] = None        # the encoder runs once per relaxation pass: keep each once
        data = bytearray(codec.encode(insn, values))
        if getattr(cand, "twin", False):
            data[1] |= 0x10
        return bytes(data), reloc

    def body_length(self, item):
        start, end = item.body
        return self.sym_offset(end) - self.sym_offset(start)

    def reloc_symbol(self, sym):
        sym.reloc_ref = True
        return sym

    # -- statements ----------------------------------------------------------------------------------------
    def instruction(self, text):
        # A and B may share a physical line; '#' belongs to A.
        if "#" in text and text.split("#", 1)[1].strip():
            first, second = text.split("#", 1)
            self.instruction(first + " #")
            self.instruction(second)
            return
        line = canon(text)
        self.stmt_serial += 1
        opens = line.endswith("{")
        par = line.endswith("#")
        good, err = self.parsed_candidates(line)
        if not good:
            # Memory operands may omit +0. Keep native bare-base syntax
            # (for example flushinv [r2]) when it already matches.
            normalized = re.sub(r"\[(r(?:1[0-5]|[0-9]))\]", r"[\1+0]", line)
            if normalized != line:
                line = normalized
                good, err = self.parsed_candidates(line)
        if par:
            # a parallel twin the table does not list: the 32-bit forms in E000..E7FF have one with bit 12 set
            base, err2 = self.parsed_candidates(canon(line[:-1]))
            have = {c.insn.id for c in good}
            for c in base:
                if len(c.insn.words) == 2 and (c.insn.value >> 16) & 0xF800 == 0xE000 \
                        and c.insn.id + ".p" not in self.isa.by_id:
                    c.twin = True
                    good.append(c)
            err = err or err2
        if not good:
            raise AsmError(f"{self.loc}: unrecognised instruction or operand: {text.strip()}"
                           + (f" ({err})" if err else ""))
        cands = good
        if self.pending_pair:
            if not any(c.insn.id in PARALLEL_B for c in cands):
                raise AsmError(f"{self.loc}: invalid parallel-B instruction")
            cands = [c for c in cands if c.insn.id in PARALLEL_B]
        # parse-time constness of the numeric operands
        fixed = all(self.is_pure(op.node) for c in cands for op in c.ops.values() if isinstance(op, Num))
        if not fixed:
            # a constant that was not known when the line was read (a later .equ, a label difference) is
            # assembled like a symbol: the 32-bit form
            wide = [c for c in cands if self.isa.abs32.get(c.insn.id)]
            if wide:
                cands = wide
        cands.sort(key=lambda c: (c.size, c.alias, TIE.get(c.insn.id.removesuffix('.p'), 0), c.order))
        item = Insn(cands, self.loc, fixed)
        item.cur = cands[0].size
        item.chosen = cands[0]
        counts_toward = self.blocks[-1] if self.blocks and self.blocks[-1].kind == "if" else None
        if opens and self.blocks:
            is_if = any(self.isa.ifblock.get(c.insn.id) for c in cands)
            if (is_if and any(b.kind == "if" for b in self.blocks)) or (not is_if and self.blocks):
                raise AsmError(f"{self.loc}: invalid location of {'it block' if is_if else 'REP'}")
        if counts_toward is not None and not self.pending_pair:
            counts_toward.counts[counts_toward.in_else] += 1
        self.pending_pair = par and not opens
        if opens:
            self.counter += 1
            start = self.syms[f".Lbody{self.counter}"] = Sym(f".Lbody{self.counter}")
            start.temp = True
            end = self.syms[f".Lbodyend{self.counter}"] = Sym(f".Lbodyend{self.counter}")
            end.temp = True
            self.cur.items.append(item)
            self.define_label(start)
            kind = "if" if any(self.isa.ifblock.get(c.insn.id) for c in cands) else "rep"
            blk = Block(kind, item, end)
            self.blocks.append(blk)
            item.body = (start, end)
            item.block = blk
            return
        self.cur.items.append(item)

    def parsed_candidates(self, line):
        """The readings of a canonical line whose numeric operands parse as expressions, and the first error."""
        good, err, feature_error = [], None, None
        for c in self.isa.candidates(line):
            if self.features is not None and not set(c.insn.requires) <= self.features:
                missing = sorted(set(c.insn.requires) - self.features)
                feature_error = AsmError(f"this form needs -m{missing[0]}")
                continue
            try:
                c.ops = {k: (Num(("un", "-", self.parse_expr(v.node, v.dots, True)) if v.neg
                                 else self.parse_expr(v.node, v.dots, True)) if isinstance(v, Num) else v)
                         for k, v in c.ops.items()}
            except AsmError as e:
                err = err or e
                continue
            good.append(c)
        return good, feature_error or err

    def else_block(self):
        if not self.blocks or self.blocks[-1].kind != "if" or self.blocks[-1].in_else:
            raise AsmError(f"{self.loc}: else without if")
        if self.blocks[-1].counts[0] == 0:
            raise AsmError(f"{self.loc}: if-clause out of range")
        self.blocks[-1].in_else = True

    def close_block(self):
        if not self.blocks:
            raise AsmError(f"{self.loc}: unmatched }}")
        b = self.blocks.pop()
        if b.kind == "if":
            then, els = b.counts
            if not 1 <= then <= 4:
                raise AsmError(f"{self.loc}: if-clause out of range")
            if els > 3 or (b.in_else and els == 0):
                raise AsmError(f"{self.loc}: else-clause out of range")
        self.define_label(b.end_label)

    def label(self, name):
        if re.fullmatch(r"\d+", name):
            slot = self.local.setdefault(name, {"def": None, "pending": None})
            s = slot["pending"] or self.new_tmp()
            slot["pending"], slot["def"] = None, s
            self.define_label(s)
            return
        s = self.sym(name)
        if s.kind == "abs" or s.kind == "alias":
            raise AsmError(f"{self.loc}: symbol {name} is already defined")
        self.define_label(s)
        s.used = True

    # -- layout --------------------------------------------------------------------------------------------
    def layout(self):
        """Choose every instruction's form: start from the smallest and grow until nothing changes."""
        for _ in range(200):
            changed = False
            for sec in self.order:
                off, offs = 0, []
                for item in sec.items:
                    offs.append(off)
                    off += item.size(self, sec, off)
                offs.append(off)
                sec.offs, sec.size = offs, off
            for s in self.syms.values():
                if s.kind == "label":
                    s.offset = s.section.offs[s.item]
            for sec in self.order:
                for idx, item in enumerate(sec.items):
                    if not isinstance(item, Insn):
                        continue
                    off = sec.offs[idx]
                    item.sec, item.off = sec, off
                    pick = None
                    for c in item.cands:
                        if c.size >= item.cur and self.encode_cand(item, c, off) is not None:
                            pick = c
                            break
                    if pick is None:
                        raise AsmError(f"{item.loc}: no encoding fits the operands of this instruction"
                                       + (f" ({item.error})" if item.error else ""))
                    item.chosen = pick
                    if pick.size != item.cur:
                        item.cur = pick.size
                        changed = True
            if not changed:
                return
        raise AsmError("layout did not converge")

    # -- output --------------------------------------------------------------------------------------------
    def finish(self):
        if self.blocks:
            raise AsmError(f"{self.loc}: block not closed with }}")
        if self.cond:
            raise AsmError(f"{self.loc}: .if without .endif")
        for n, slot in self.local.items():
            if slot["pending"] is not None:
                raise AsmError(f"directional label {n}f is never defined")
        self.layout()
        outs = {}
        for sec in self.order:
            o = OutSec()
            for idx, item in enumerate(sec.items):
                off = sec.offs[idx]
                if sec.type == elf.SHT_NOBITS:
                    if isinstance(item, (Space, Align, Org)):
                        if isinstance(item, Space) and item.fill and self.ev(item.value, item.loc)[1] & M64:
                            raise AsmError(f"{item.loc}: can't put non-zero data in the NOBITS section {sec.name}")
                        continue
                    # a branch with several sizes is a relaxable fragment in JieLi's assembler, which no check looks at
                    branch = isinstance(item, Insn) and len({c.size for c in item.cands
                                                             if self.isa.branch.get(c.insn.id)}) > 1
                    scratch = o if branch else OutSec()         # branches are relaxable fragments: not checked
                    item.emit(self, sec, off, scratch)
                    if not branch and (any(scratch.data) or scratch.relocs):
                        raise AsmError(f"{item.loc}: can't put non-zero data in the NOBITS section {sec.name}")
                    continue
                item.emit(self, sec, off, o)
            outs[sec.name] = o
        # symbols
        symbols = []
        for s in self.syms.values():
            if s.kind == "undef" and not (s.reloc_ref or s.declared):
                continue
            if s.temp and not s.reloc_ref:
                continue
            symbols.append(s)
        result = []
        for s in symbols:
            e = elf.Symbol(s.name, bind=s.bind, type=s.type, other=s.other)
            size_node = s.size_node
            if size_node is None and s.kind == "alias":      # an alias has its base symbol's size
                base = self.alias_base(s)
                size_node = base.size_node if base is not None else None
            if size_node is not None:
                sym, v = self.ev(size_node, None)
                e.size = v & M32
            if s.common:
                e.shndx, e.value, e.size = elf.SHN_COMMON, s.common[1], s.common[0]
                e.bind = elf.STB_GLOBAL
                e.type = elf.STT_OBJECT
            elif s.kind in ("label", "alias"):
                if s.kind == "alias" and s.type == elf.STT_NOTYPE:      # an alias takes its base symbol's type
                    base = self.alias_base(s)
                    if base is not None:
                        e.type = base.type
                p = self.sym_pos(s)
                if p is None:
                    raise AsmError(f"symbol {s.name} refers to an undefined symbol")
                e.shndx, e.value = p[0].name, p[1]
            elif s.kind == "abs":
                e.shndx, e.value = elf.SHN_ABS, s.value & M32
            elif s.kind == "undef" and s.bind == elf.STB_LOCAL:
                e.bind = elf.STB_GLOBAL
            result.append(e)
        for sec in self.order:             # JieLi's assembler also emits a symbol named after each TLS section
            if sec.flags & elf.SHF_TLS and sec.name not in self.syms:
                result.append(elf.Symbol(sec.name, type=elf.STT_TLS, shndx=sec.name))
        result.sort(key=lambda e: (e.bind != elf.STB_LOCAL, e.name.encode()))
        for name in reversed(self.file_names):
            result.insert(0, elf.Symbol(name, bind=elf.STB_LOCAL, type=elf.STT_FILE, shndx=elf.SHN_ABS))
        sections = []
        for sec in self.order:
            o = outs[sec.name]
            e = elf.Section(sec.name, sec.type, sec.flags, sec.align, sec.entsize)
            if sec.type == elf.SHT_NOBITS:
                e.size = sec.size
            else:
                e.data = o.data
            e.relocs = o.relocs
            sections.append(e)
        return elf.write(sections, result)


# `(u)` at the end of a line is optional (unsigned is the default) except for these entries (found by leaving it
# off every entry's examples): JieLi's assembler refuses the line without it.
U_REQUIRED = {
    "madd.mem2.hh.i.u", "madd.mem2.hh.rh.u", "madd.mul4.u.prr", "msub.mem2.hh.i.u", "msub.mem2.hh.rh.u",
    "mul.mem2.hh.i.u", "mul.mem2.hh.rh.u", "mul4.u.prr", "sum.acc.u.prhrhrhrh", "sum.nacc.u.prhrhrhrh",
    "sum.u.prhrhrhrh",
}

# The instructions that may follow a parallel-issue (`#`) instruction: JieLi's assembler refuses any other with
# "invalid parallel-B instruction" (found by trying every entry's examples after `r0 += r7 #`).
PARALLEL_B = {
    "ld.bu", "ld.bu.l", "ld.bu.pi", "ld.bu.pi.l", "ld.bu.pin", "ld.bu.pin.l", "ld.hu", "ld.hu.l", "ld.hu.pi",
    "ld.hu.pi.l", "ld.hu.pin", "ld.hu.pin.l", "ld.w", "ld.w.l", "ld.w.pi", "ld.w.pi.l", "ld.w.pin",
    "ld.w.pin.l", "ld.w.sp", "ld.w.sp.l", "pop.pc", "st.b", "st.b.l", "st.b.pi", "st.b.pi.l", "st.b.pin",
    "st.b.pin.l", "st.h", "st.h.l", "st.h.pi", "st.h.pi.l", "st.h.pin", "st.h.pin.l", "st.w", "st.w.l",
    "st.w.pi", "st.w.pi.l", "st.w.pin", "st.w.pin.l", "st.w.sp", "st.w.sp.l",
}

# When several entries encode a line in the same number of bytes, JieLi's assembler picks the lowest value here
# (default 0, then table order). Found with `oracle encode`; see docs/ASSEMBLER.md.
TIE = {
    "li.s16": -1,           # over li.modimm
    "add.s14.rri": -1,      # over add.modimm.rri
    "add.rr": 1,            # add.rrr is preferred for `r1 += r2`
    "pop.pc_rmask": 1,      # `{pc} = [sp++]` is pop.srmask
    "push.rets_rmask.c": 1,
}
TIE.update({f"{c}.ri.l.modimm": 1 for c in ("beq", "bne", "bge", "blt", "bgt", "ble", "bges", "blts", "bgts", "bles")})


_ISA_CACHE = {}
_DEFAULT = []


def _default_table():
    """The table in isa/pi32v2.toml, loaded once."""
    if not _DEFAULT:
        _DEFAULT.append(tbl.load())
    return _DEFAULT[0]


def _isa_for(tab):
    key = id(tab)
    if key not in _ISA_CACHE:
        _ISA_CACHE[key] = Isa(tab)
    return _ISA_CACHE[key]


# --- source processing: statements, directives, macros ------------------------------------------------------

LABEL = re.compile(rf"\s*({IDENT}|\d+)\s*:(?!=)")
SUBST = re.compile(r"\\(\(\)|@|[A-Za-z_][A-Za-z0-9_]*)")


def split_args(text, seps=","):
    """Split at top-level separators, outside quotes and brackets."""
    out, depth, cur, quote, i = [], 0, [], None, 0
    while i < len(text):
        c = text[i]
        if quote:
            cur.append(c)
            if c == "\\" and i + 1 < len(text):
                cur.append(text[i + 1])
                i += 1
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
            cur.append(c)
        elif c in "([{":
            depth += 1
            cur.append(c)
        elif c in ")]}":
            depth -= 1
            cur.append(c)
        elif c in seps and depth == 0:
            out.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    out.append("".join(cur))
    return [a.strip() for a in out]


def parse_string(text):
    """The bytes of a "quoted" string with C escapes."""
    text = text.strip()
    if len(text) < 2 or text[0] != '"' or text[-1] != '"':
        raise AsmError(f"expected a quoted string, found {text!r}")
    body, out, i = text[1:-1], bytearray(), 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out += c.encode()
            i += 1
            continue
        i += 1
        c = body[i:i + 1]
        if c in STRING_ESC and c != "":
            out.append(STRING_ESC[c])
            i += 1
        elif c.isdigit() and c in "01234567":
            j = i
            while j < len(body) and j < i + 3 and body[j] in "01234567":
                j += 1
            out.append(int(body[i:j], 8) & 0xFF)
            i = j
        elif c in ("x", "X"):
            j = i + 1
            while j < len(body) and body[j] in "0123456789abcdefABCDEF":
                j += 1
            if j == i + 1:
                raise AsmError("invalid escape sequence in string")
            out.append(int(body[i + 1:j], 16) & 0xFF)
            i = j
        else:
            raise AsmError("invalid escape sequence (unrecognized character) in string")
    return bytes(out)


class Assembler(_Core):
    """Assemble preprocessed source text. Typical use: `Assembler().assemble(text, "x.s")` then `.finish()`."""

    def __init__(self, tab=None, include=(), features=None, chip=None, chip_errors=False):
        super().__init__(tab, include, features, chip, chip_errors)
        self.stopped = False
        self.cur_file = "<input>"

    # -- driver --------------------------------------------------------------------------------------------
    def assemble(self, text, filename="<input>"):
        self.cur_file = filename
        lines = strip_comments(text).split("\n")
        self.run(lines, filename, 1)

    def run(self, lines, filename, first_line):
        i = 0
        while i < len(lines) and not self.stopped:
            self.loc = f"{filename}:{first_line + i}"
            line = lines[i]
            i += 1
            word = line.split(None, 1)[0].lower() if line.strip() else ""
            if self.cond and not self.cond[-1][0] and not word.startswith((".if", ".else", ".endif", ".elseif")):
                if word in (".macro", ".rept", ".irp", ".irpc", ".rep"):
                    i = self.collect(lines, i, word)[1]
                continue
            if word in (".macro", ".rept", ".rep", ".irp", ".irpc"):
                body, i2 = self.collect(lines, i, word)
                self.start_block(word, line, body, filename, first_line + i)
                i = i2
                continue
            self.statement(line)

    def collect(self, lines, i, opener):
        """The body lines up to the matching .endm/.endr; returns (body, next index)."""
        depth, body = 1, []
        starts = (".macro",) if opener == ".macro" else (".rept", ".rep", ".irp", ".irpc")
        end = ".endm" if opener == ".macro" else ".endr"
        while i < len(lines):
            w = lines[i].split(None, 1)[0].lower() if lines[i].strip() else ""
            if w in starts:
                depth += 1
            elif w == end or (opener == ".macro" and w == ".endmacro"):
                depth -= 1
                if depth == 0:
                    return body, i + 1
            body.append(lines[i])
            i += 1
        raise AsmError(f"{self.loc}: missing {end}")

    def start_block(self, word, line, body, filename, lineno):
        rest = line.split(None, 1)[1] if len(line.split(None, 1)) > 1 else ""
        if word == ".macro":
            name, *params = [p for p in re.split(r"[,\s]+", rest.strip()) if p]
            defaults, plist = {}, []
            for p in params:
                pn, _, dv = p.partition("=")
                pn = pn.split(":")[0]
                plist.append(pn)
                if dv:
                    defaults[pn] = dv
            self.macros[name] = (plist, defaults, body)
            return
        if word in (".rept", ".rep"):
            n = self.const_expr(rest, "repeat count")
            for _ in range(max(n, 0)):
                self.run(body, filename, lineno)
            return
        args = split_args(rest)
        var = args[0]
        items = args[1:] if word == ".irp" else list(args[1] if len(args) > 1 else "")
        for it in items:
            sub = [re.sub(r"\\" + re.escape(var) + r"(?![A-Za-z0-9_])", it.replace("\\", "\\\\"), ln) for ln in body]
            self.run(sub, filename, lineno)

    def expand_macro(self, name, argtext):
        plist, defaults, body = self.macros[name]
        args = [a for a in split_args(argtext, ",") if a != ""] if argtext.strip() else []
        if len(args) == 1 and " " in args[0] and len(plist) > 1 and not args[0].startswith('"'):
            args = args[0].split()
        values = dict(defaults)
        for k, a in enumerate(args):
            if "=" in a and a.split("=", 1)[0] in plist:
                key, _, v = a.partition("=")
                values[key] = v
            elif k < len(plist):
                values[plist[k]] = a
            else:
                raise AsmError(f"{self.loc}: too many arguments for macro {name}")
        inst = self.macro_count            # \@ counts every macro invocation, from 0
        self.macro_count += 1

        def sub(m):
            tok = m.group(1)
            if tok == "()":
                return ""
            if tok == "@":
                return str(inst)
            return values.get(tok, "")
        lines = [SUBST.sub(sub, ln) for ln in body]
        self.run(lines, self.loc.rsplit(":", 1)[0], int(self.loc.rsplit(":", 1)[1]))

    # -- one statement -------------------------------------------------------------------------------------
    def statement(self, line):
        text = line.strip()
        if not text:
            return
        active = not self.cond or self.cond[-1][0]
        if active:
            while True:
                m = LABEL.match(text)
                if not m:
                    break
                self.label(m.group(1))
                text = text[m.end():].strip()
            if not text:
                return
        word = text.split(None, 1)[0]
        rest = text[len(word):].strip()
        lw = word.lower()
        if lw.startswith((".if", ".else", ".endif", ".elseif")):
            return self.conditional(lw, rest)
        if not active:
            return
        if text == "}":
            return self.close_block()
        if re.fullmatch(r"\}\s*else\s*\{", text):
            return self.else_block()
        if word in self.macros:
            return self.expand_macro(word, rest)
        if word.startswith("."):
            return self.directive(lw, rest)
        self.instruction(text)

    def conditional(self, w, rest):
        if w == ".endif":
            if not self.cond:
                raise AsmError(f"{self.loc}: .endif without .if")
            self.cond.pop()
            return
        parent = not self.cond or self.cond[-1][0]
        if w in (".else", ".elseif"):
            if not self.cond:
                raise AsmError(f"{self.loc}: {w} without .if")
            _, taken, par = self.cond.pop()
            if w == ".else":
                self.cond.append([par and not taken, True, par])
            else:
                ok = par and not taken and self.truth(".if", rest)
                self.cond.append([ok, taken or ok, par])
            return
        ok = parent and self.truth(w, rest)
        self.cond.append([ok, ok, parent])

    def truth(self, w, rest):
        if w == ".ifdef" or w == ".ifndef":
            s = self.syms.get(rest.strip())
            d = (s is not None and s.kind != "undef") or rest.strip() in self.macros
            return d if w == ".ifdef" else not d
        if w in (".ifc", ".ifnc"):
            a = split_args(rest)
            eq = len(a) == 2 and a[0] == a[1]
            return eq if w == ".ifc" else not eq
        if w in (".ifb", ".ifnb"):
            blank = not rest.strip()
            return blank if w == ".ifb" else not blank
        v = self.const_expr(rest, "condition") if rest.strip() else 0
        return {".if": v != 0, ".ifne": v != 0, ".ifeq": v == 0, ".ifge": v >= 0, ".ifgt": v > 0,
                ".ifle": v <= 0, ".iflt": v < 0}[w]

    # -- directives ----------------------------------------------------------------------------------------
    def directive(self, d, rest):
        fn = getattr(self, "d_" + d[1:].replace(".", "_"), None)
        if fn is None:
            raise AsmError(f"{self.loc}: unknown directive {d}")
        fn(rest)

    def add(self, item):
        item.loc = self.loc
        self.cur.items.append(item)

    # sections
    def d_text(self, rest):
        self.switch(".text")

    def d_data(self, rest):
        self.switch(".data")

    def d_bss(self, rest):
        self.switch(".bss")

    def d_rodata(self, rest):
        self.switch(".rodata")

    def d_section(self, rest, push=False):
        args = split_args(rest)
        name = args[0].strip('"') if args[0].startswith('"') else args[0]
        if push:
            self.stack.append(self.cur)
        flags_s, type_s = None, None
        if len(args) > 1 and args[1]:
            flags_s = args[1].strip('"')
        if len(args) > 2 and args[2]:
            type_s = args[2].lstrip("@%")
        if flags_s is None:
            self.switch(name)
            return
        flags = 0
        for ch in flags_s:
            if ch not in SECTION_FLAGS:
                raise AsmError(f"{self.loc}: unknown section flag {ch!r}")
            flags |= SECTION_FLAGS[ch]
        typ = SECTION_TYPES.get(type_s, elf.SHT_PROGBITS) if type_s else elf.SHT_PROGBITS
        if type_s and type_s not in SECTION_TYPES:
            raise AsmError(f"{self.loc}: unknown section type {type_s}")
        entsize = 0
        if flags & elf.SHF_MERGE:
            if len(args) < 4:
                raise AsmError(f"{self.loc}: mergeable section needs an entry size")
            entsize = self.const_expr(args[3], "entry size")
        self.switch(name, typ, flags, entsize)

    def d_pushsection(self, rest):
        self.d_section(rest, push=True)

    def d_popsection(self, rest):
        if not self.stack:
            raise AsmError(f"{self.loc}: .popsection without corresponding .pushsection")
        self.cur = self.stack.pop()

    def d_previous(self, rest):
        if self.prev is not None:
            self.prev, self.cur = self.cur, self.prev

    def d_subsection(self, rest):
        pass

    # symbol attributes
    def _each_name(self, rest):
        for n in split_args(rest):
            if n:
                yield self.sym(n)

    def d_globl(self, rest):
        for s in self._each_name(rest):
            s.bind, s.declared = elf.STB_GLOBAL, True

    d_global = d_globl

    def d_weak(self, rest):
        for s in self._each_name(rest):
            s.bind, s.declared = elf.STB_WEAK, True

    def d_local(self, rest):
        for s in self._each_name(rest):
            s.bind, s.local_decl = elf.STB_LOCAL, True

    def d_hidden(self, rest):
        for s in self._each_name(rest):
            s.other = 2

    def d_protected(self, rest):
        for s in self._each_name(rest):
            s.other = 3

    def d_internal(self, rest):
        for s in self._each_name(rest):
            s.other = 1

    def d_extern(self, rest):
        pass

    def d_type(self, rest):
        name, _, t = rest.partition(",")
        t = t.strip().lstrip("@%").strip()
        if t not in SYMBOL_TYPES:
            raise AsmError(f"{self.loc}: unknown symbol type {t!r}")
        self.sym(name.strip()).type = SYMBOL_TYPES[t]

    def d_size(self, rest):
        name, _, e = rest.partition(",")
        self.sym(name.strip()).size_node = self.parse_expr(e)

    def d_file(self, rest):
        if rest.strip().startswith('"'):
            self.file_names.append(parse_string(rest).decode())

    def d_ident(self, rest):
        sec = self.sections.get(".comment")
        if sec is None:
            saved = self.cur
            sec = self.switch(".comment", elf.SHT_PROGBITS, elf.SHF_MERGE | elf.SHF_STRINGS, 1)
            self.cur = saved
            sec.items.append(Raw(b"\0"))
        sec.items.append(Raw(parse_string(rest) + b"\0"))

    def d_weakref(self, rest):
        pass

    def d_line(self, rest):
        pass

    def d_end(self, rest):
        self.stopped = True

    def d_error(self, rest):
        raise AsmError(f"{self.loc}: {parse_string(rest).decode() if rest.strip().startswith(chr(34)) else rest}")

    def d_warning(self, rest):
        self.warnings.append(f"{self.loc}: warning: {rest}")

    def d_abort(self, rest):
        raise AsmError(f"{self.loc}: .abort detected")

    def d_comm(self, rest):
        a = split_args(rest)
        s = self.sym(a[0])
        size = self.const_expr(a[1], "size")
        align = self.const_expr(a[2], "alignment") if len(a) > 2 else 1
        if s.local_decl:                # `.local x` then `.comm x,n,a` allocates in .bss
            saved = self.cur
            sec = self.switch(".bss")
            sec.align = max(sec.align, align)
            self.add(Align(align, 0, None, self.loc))
            self.define_label(s)
            s.type = elf.STT_OBJECT
            s.size_node = ("num", size)
            self.add(Space(("num", size), 1, None, self.loc))
            self.cur = saved
            return
        if s.common and s.common != (size, align):
            raise AsmError(f"{self.loc}: symbol {s.name} redeclared as different type")
        s.common = (size, align)
        s.bind, s.declared, s.kind = elf.STB_GLOBAL, True, "abs"
        s.kind = "undef"
        s.type = elf.STT_OBJECT

    def d_lcomm(self, rest):
        a = split_args(rest)
        if len(a) > 2:
            raise AsmError(f"{self.loc}: alignment not supported on this target")
        s = self.sym(a[0])
        size = self.const_expr(a[1], "size")
        saved = self.cur
        self.switch(".bss")
        self.define_label(s)
        s.type = elf.STT_OBJECT
        s.size_node = ("num", size)
        self.add(Space(("num", size), 1, None, self.loc))
        self.cur = saved

    def d_equ(self, rest):
        name, _, e = rest.partition(",")
        name = name.strip()
        s = self.sym(name)
        if s.kind in ("label",):
            raise AsmError(f"{self.loc}: symbol {name} is already defined")
        node = self.parse_expr(e)
        pure = self.is_pure(node)
        try:
            sym, v = self.ev(node, self.loc, provisional=True)
        except AsmError:
            sym, v = ..., 0
        if sym is None:
            s.kind, s.value, s.pure = "abs", v, pure
        else:
            s.kind, s.node, s.pure = "alias", node, False

    d_set = d_equ
    d_equiv = d_equ

    # data
    def _data(self, rest, width):
        for a in split_args(rest):
            if a == "":
                raise AsmError(f"{self.loc}: missing expression in data directive")
            self.add(Data(width, self.parse_expr(a), self.loc))

    def d_byte(self, rest):
        self._data(rest, 1)

    def d_short(self, rest):
        self._data(rest, 2)

    d_2byte = d_short

    def d_long(self, rest):
        self._data(rest, 4)

    d_int = d_long
    d_4byte = d_long

    def d_quad(self, rest):
        self._data(rest, 8)

    d_8byte = d_quad

    def _float(self, rest, fmt):
        for a in split_args(rest):
            try:
                v = float(a)
            except ValueError:
                raise AsmError(f"{self.loc}: bad floating-point number {a!r}") from None
            self.add(Raw(struct.pack("<" + fmt, v)))

    def d_float(self, rest):
        self._float(rest, "f")

    d_single = d_float

    def d_double(self, rest):
        self._float(rest, "d")

    def d_ascii(self, rest, zero=False):
        for a in split_args(rest):
            self.add(Raw(parse_string(a) + (b"\0" if zero else b"")))

    def d_asciz(self, rest):
        self.d_ascii(rest, True)

    d_string = d_asciz

    def d_uleb128(self, rest):
        for a in split_args(rest):
            self.add(Leb(self.parse_expr(a), False, self.loc))

    def d_sleb128(self, rest):
        for a in split_args(rest):
            self.add(Leb(self.parse_expr(a), True, self.loc))

    def d_space(self, rest):
        a = split_args(rest)
        self.add(Space(self.parse_expr(a[0]), 1, self.parse_expr(a[1]) if len(a) > 1 else None, self.loc, True))

    d_skip = d_space

    def d_zero(self, rest):
        a = split_args(rest)
        self.add(Space(self.parse_expr(a[0]), 1, self.parse_expr(a[1]) if len(a) > 1 else None, self.loc, True))

    def d_fill(self, rest):
        a = split_args(rest)
        size = self.const_expr(a[1], "size") if len(a) > 1 else 1
        value = self.parse_expr(a[2]) if len(a) > 2 else ("num", 0)
        self.add(Space(self.parse_expr(a[0]), min(size, 8) if size else 1, value, self.loc, fill=True))

    def _align(self, rest, power):
        a = split_args(rest)
        n = self.const_expr(a[0], "alignment")
        boundary = (1 << n) if power else n
        if boundary <= 0 or boundary & (boundary - 1):
            raise AsmError(f"{self.loc}: alignment must be a power of 2")
        fill = self.const_expr(a[1], "fill") & 0xFF if len(a) > 1 and a[1] else 0
        maxskip = self.const_expr(a[2], "max skip") if len(a) > 2 and a[2] else None
        self.cur.align = max(self.cur.align, boundary)
        self.add(Align(boundary, fill, maxskip, self.loc))

    def d_align(self, rest):
        self._align(rest, False)

    d_balign = d_align
    d_balignw = d_align
    d_balignl = d_align

    def d_p2align(self, rest):
        self._align(rest, True)

    d_p2alignw = d_p2align
    d_p2alignl = d_p2align

    def d_org(self, rest):
        a = split_args(rest)
        fill = self.const_expr(a[1], "fill") & 0xFF if len(a) > 1 else 0
        self.add(Org(self.parse_expr(a[0]), fill, self.loc))

    def _find(self, name):
        base = os.path.dirname(self.cur_file) or "."
        for d in [base, *self.include, "."]:
            p = os.path.join(d, name)
            if os.path.exists(p):
                return p
        return None

    def d_incbin(self, rest):
        a = split_args(rest)
        name = parse_string(a[0]).decode()
        p = self._find(name)
        if p is None:
            raise AsmError(f"{self.loc}: Could not find incbin file '{name}'")
        data = open(p, "rb").read()
        skip = self.const_expr(a[1], "skip") if len(a) > 1 else 0
        count = self.const_expr(a[2], "count") if len(a) > 2 else None
        data = data[skip:] if count is None else data[skip:skip + count]
        self.add(Raw(data))

    def d_include(self, rest):
        name = parse_string(rest).decode()
        p = self._find(name)
        if p is None:
            raise AsmError(f"{self.loc}: Could not find include file '{name}'")
        saved = self.cur_file
        self.cur_file = p
        self.run(strip_comments(open(p).read()).split("\n"), p, 1)
        self.cur_file = saved


def assemble_source(text, filename="<input>", include=(), tab=None, features=None, chip=None, chip_errors=False,
                    warnings=None):
    """Assemble preprocessed source text; returns the ELF object as bytes. `features` (e.g. {"instrev3"})
    restricts the entries to those whose `requires` it covers; by default every entry is allowed. With `chip`
    (e.g. "ac791n"), forms the table's `hw` key marks on that chip are reported: appended to `warnings` (a list),
    or refused with `chip_errors`."""
    a = Assembler(tab, include, features, chip, chip_errors)
    a.assemble(text, filename)
    data = a.finish()
    if warnings is not None:
        warnings.extend(a.hw_warnings)
    return data


def preprocess(path, include=(), defines=()):
    """Run the system C preprocessor (/usr/bin/cpp) over an assembly file (.S) and return its output."""
    cmd = ["/usr/bin/cpp", "-P", "-x", "assembler-with-cpp"]
    for d in include:
        cmd += ["-I", str(d)]
    for d in defines:
        cmd += ["-D", d]
    r = subprocess.run(cmd + [str(path)], capture_output=True, text=True)
    if r.returncode:
        raise AsmError(f"cpp failed: {r.stderr.strip()}")
    return r.stdout


def main(args):
    """Entry for `python3 -m pi32v2isa as IN.s -o OUT.o [--cpp] [-I DIR] [-D NAME[=VALUE]] [--chip NAME
    [--chip-errors]]`."""
    path = args.input
    include = [os.path.dirname(os.path.abspath(path))] + list(args.include)
    if args.cpp or path.endswith(".S"):
        text = preprocess(path, args.include, args.define)
    else:
        if path == "-":
            text = sys.stdin.read()
        else:
            with open(path) as f:
                text = f.read()
    warnings = []
    try:
        data = assemble_source(text, path, include, features=args.feature or None,
                               chip=getattr(args, "chip", None), chip_errors=getattr(args, "chip_errors", False),
                               warnings=warnings)
    except AsmError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    for w in warnings:
        print(f"warning: {w}", file=sys.stderr)
    out = args.output or (os.path.splitext(os.path.basename(path))[0] + ".o")
    with open(out, "wb") as f:
        f.write(data)
    return 0
