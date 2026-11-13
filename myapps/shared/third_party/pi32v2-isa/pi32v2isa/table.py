# SPDX-FileCopyrightText: 2026 Julius Vitkauskas
# SPDX-License-Identifier: MIT-0
"""Load and check the instruction table (docs/TABLE-FORMAT.md)."""

import re
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TABLE = ROOT / "isa" / "pi32v2.toml"
SOURCES = ROOT / "docs" / "SOURCES.md"

STATUSES = ("guessed", "documented", "confirmed")
# JieLi objdump's -mcpu models ("generic" = no -mcpu) and the features each one implies (docs/TABLE-FORMAT.md)
CPU_MODELS = ("generic", "r1", "r2", "r3", "r4", "r5", "r2v3", "r3v3", "r4v3", "r4v3s")
CPU_FEATURES = {"r3": ("fprev1",), "r4": ("fprev1",), "r5": ("fprev1",), "r3v3": ("fprev1",),
                "r4v3": ("fprev1",), "r4v3s": ("fprev1",)}
# Chips whose behaviour the table records with `hw` (docs/TABLE-FORMAT.md, "Hardware behaviour"): what a form
# does on that chip when it is not what its syntax says.
CHIPS = {"ac791n": "JieLi AC791N (M-VAVE FM-1), hardware runs hw/fm1-2026-10-24*"}
HW_KINDS = {"resets": "resets the chip", "nop": "does nothing", "unpredictable": "gives unpredictable results"}
WORD_CHARS = re.compile(r"^[01a-z-]{16}$")
# In syntax: {x} is field x, {+x} prints "+value" or nothing when the value is 0, {{ and }} are literal braces.
SYNTAX_TOKEN = re.compile(r"\{\{|\}\}|\{(\+?)([a-z])\}")

# kind -> allowed field widths, or None for any width >= 1 (docs/TABLE-FORMAT.md)
KINDS = {
    "gpr": {4},
    "gpr_lo": {3},
    "gpr_hi": {3},
    "pair": {3},
    "pair_regs": {5},
    "sr": {4},
    "uimm": None,
    "simm": None,
    "bit": {5},
    "invbit": {5},
    "rlist4": {4},
    "rmask": {16},
    "srmask": None,
    "modimm": {12},
    "enum": None,
}
NUMERIC = ("uimm", "simm")      # the kinds `scale` and `add` apply to


def placeholders(syntax):
    """The field names a syntax string uses, in order."""
    return [m.group(2) for m in SYNTAX_TOKEN.finditer(syntax) if m.group(2)]


@dataclass
class Field:
    kind: str
    letters: str            # encoding letters that hold the field, most significant part first
    scale: int = 1          # printed value = raw (sign-extended for simm) * scale + add
    add: int = 0
    values: list[str] = field(default_factory=list)   # enum only: the text for each raw value
    invert: bool = False    # modimm only: the complement of the modified immediate
    decimal: bool = False   # modimm only: printed as a signed decimal instead of hex
    lead: str = ""          # rmask only: a register name printed first, before the list
    zero_as: int | None = None   # uimm only: the number the raw value 0 stands for
    asm_range: list[int] = field(default_factory=list)   # uimm/simm: [lo, hi] the assembler accepts, if narrower


@dataclass
class Example:
    bytes: bytes
    text: str
    origin: str


@dataclass
class Insn:
    id: str
    syntax: str
    words: list[str]
    fields: dict[str, Field]
    summary: str
    status: str
    sources: list[str]
    group: str = ""
    requires: list[str] = field(default_factory=list)          # features the assembler needs for it
    decode_requires: list[str] = field(default_factory=list)   # features without which objdump does not show it
    absent_on: list[str] = field(default_factory=list)         # CPU models whose objdump does not show it
    notes: str = ""
    examples: list[Example] = field(default_factory=list)
    asm_rejects: list[str] = field(default_factory=list)   # texts objdump prints but the assembler refuses
    asm_no_syntax: bool = False   # every printed text is refused or emits no instruction
    asm_wrong: list[str] = field(default_factory=list)     # texts JieLi's assembler is known to mis-assemble
    asm_syntax: str = ""    # how the assembler wants the line, when it differs from objdump's syntax
    body: str = ""          # a block opener: the field whose value is the body's length in bytes
    block: dict[str, str] = field(default_factory=dict)  # if-block opener: {"then": f, "else": f} implied fields
    hw: dict[str, str] = field(default_factory=dict)     # chip -> "resets", "nop", "unpredictable" or "as:<id>"

    @property
    def nbits(self):
        return 16 * len(self.words)

    def _bits(self):
        """The encoding as one string, first word's most significant bit first."""
        return "".join(self.words)

    @property
    def mask(self):
        return int("".join("1" if c in "01" else "0" for c in self._bits()), 2)

    @property
    def value(self):
        return int("".join("1" if c == "1" else "0" for c in self._bits()), 2)

    def field_bits(self, name):
        """Bit positions of a field, most significant first, counted from the high end of the encoding."""
        bits = self._bits()
        return [i for letter in self.fields[name].letters for i, c in enumerate(bits) if c == letter]

    def letters(self):
        return sorted({c for c in self._bits() if c.isalpha()})


@dataclass
class Table:
    format: int
    insns: list[Insn]

    def by_id(self, insn_id):
        return next(i for i in self.insns if i.id == insn_id)


def source_ids(path=SOURCES):
    """The source ids listed in docs/SOURCES.md: the first column of its table."""
    return {m.group(1) for m in re.finditer(r"^\| `([a-z0-9-]+)` \|", path.read_text(), re.M)}


def _parse_bytes(text):
    return bytes(int(b, 16) for b in text.split())


def _field(name, spec):
    if isinstance(spec, str):
        return Field(kind=spec, letters=name)
    return Field(kind=spec.get("kind", ""), letters=spec.get("letters", name),
                 scale=spec.get("scale", 1), add=spec.get("add", 0), values=list(spec.get("values", [])),
                 invert=spec.get("invert", False), decimal=spec.get("decimal", False),
                 lead=spec.get("lead", ""), zero_as=spec.get("zero_as"), asm_range=list(spec.get("asm_range", [])))


def load(path=TABLE):
    raw = tomllib.loads(Path(path).read_text())
    insns = []
    for e in raw.get("insn", []):
        insns.append(Insn(
            id=e.get("id", ""),
            syntax=e.get("syntax", ""),
            words=list(e.get("encoding", [])),
            fields={name: _field(name, spec) for name, spec in e.get("fields", {}).items()},
            summary=e.get("summary", ""),
            status=e.get("status", ""),
            sources=list(e.get("sources", [])),
            group=e.get("group", ""),
            requires=list(e.get("requires", [])),
            decode_requires=list(e.get("decode_requires", [])),
            absent_on=list(e.get("absent_on", [])),
            notes=e.get("notes", ""),
            examples=[Example(_parse_bytes(x.get("bytes", "")), x.get("text", ""), x.get("origin", ""))
                      for x in e.get("example", [])],
            asm_rejects=list(e.get("asm_rejects", [])),
            asm_wrong=list(e.get("asm_wrong", [])),
            asm_no_syntax=e.get("asm_no_syntax", False),
            asm_syntax=e.get("asm_syntax", ""),
            body=e.get("body", ""),
            block=dict(e.get("block", {})),
            hw=dict(e.get("hw", {})),
        ))
    return Table(format=raw.get("format", 0), insns=insns)


def record_confirmation(path, insn_id, examples, source="oracle"):
    """Mark an entry confirmed in the table file, add `source` and append examples [(bytes, text, origin)].
    Edits the text in place so the file's layout and comments stay as they are."""
    path = Path(path)
    lines = path.read_text().splitlines(keepends=True)
    starts = [n for n, ln in enumerate(lines) if ln.startswith("[[insn]]")] + [len(lines)]
    for a, b in zip(starts, starts[1:]):
        if f'id = "{insn_id}"\n' in lines[a:b]:
            break
    else:
        raise KeyError(insn_id)
    block = lines[a:b]
    end = len(block)        # trailing blank and comment lines belong to the next entry
    while end and (not block[end - 1].strip() or block[end - 1].startswith("#")):
        end -= 1
    body, tail = block[:end], block[end:]
    have = {x["bytes"] for x in tomllib.loads("".join(body)).get("insn", [{}])[0].get("example", [])}
    for n, ln in enumerate(body):
        if ln.startswith("status = "):
            body[n] = 'status = "confirmed"\n'
        elif ln.startswith("sources = "):
            current = tomllib.loads(ln)["sources"]
            if source not in current:
                current.append(source)
            body[n] = "sources = [" + ", ".join(f'"{s}"' for s in current) + "]\n"
    for data, text, origin in examples:
        if data.hex(" ") in have:
            continue
        body += ["\n", "[[insn.example]]\n", f'bytes = "{data.hex(" ")}"\n', f'text = "{text}"\n',
                 f'origin = "{origin}"\n']
    lines[a:b] = body + tail
    path.write_text("".join(lines))


def _overlap(a, b):
    """True if some instruction stream matches both entries: compare the words they have in common."""
    n = min(len(a.words), len(b.words))
    shift_a, shift_b = 16 * (len(a.words) - n), 16 * (len(b.words) - n)
    ma, va = a.mask >> shift_a, a.value >> shift_a
    mb, vb = b.mask >> shift_b, b.value >> shift_b
    return (va ^ vb) & ma & mb == 0


def check(table, sources=None):
    """Return a list of problems; an empty list means the table is consistent."""
    from . import codec

    sources = source_ids() if sources is None else sources
    errors = []
    if table.format != 1:
        errors.append(f"format is {table.format!r}, expected 1")
    seen = set()
    ids = {i.id for i in table.insns}
    for i in table.insns:
        where = i.id or "<no id>"
        for key in ("id", "syntax", "words", "summary", "status", "sources"):
            if not getattr(i, key):
                errors.append(f"{where}: missing {'encoding' if key == 'words' else key}")
        if i.id in seen:
            errors.append(f"{where}: duplicate id")
        seen.add(i.id)
        for w in i.words:
            if not WORD_CHARS.match(w):
                errors.append(f"{where}: encoding word {w!r} is not 16 characters of 0, 1, -, a-z")
        used = set(i.letters())
        owners = {}
        for name, f in i.fields.items():
            for letter in f.letters:
                if letter in owners:
                    errors.append(f"{where}: letter {letter!r} belongs to fields {owners[letter]!r} and {name!r}")
                owners[letter] = name
                if letter not in used:
                    errors.append(f"{where}: field {name!r} uses letter {letter!r}, which is not in the encoding")
        for letter in used - owners.keys():
            errors.append(f"{where}: letter {letter!r} in the encoding belongs to no field")
        for name, f in i.fields.items():
            if f.kind not in KINDS:
                errors.append(f"{where}: unknown kind {f.kind!r} for field {name!r}")
                continue
            width = len(i.field_bits(name))
            allowed = KINDS[f.kind]
            if width and allowed is not None and width not in allowed:
                errors.append(f"{where}: field {name!r} is {width} bits, kind {f.kind} needs {sorted(allowed)}")
            if f.kind == "simm" and width == 1:
                errors.append(f"{where}: field {name!r} is a 1-bit simm")
            if f.kind == "enum" and len(f.values) != 1 << width:
                errors.append(f"{where}: enum field {name!r} is {width} bits and needs {1 << width} values, has {len(f.values)}")
            if (f.invert or f.decimal) and f.kind != "modimm":
                errors.append(f"{where}: field {name!r}: invert and decimal apply only to modimm")
            if f.lead and f.kind != "rmask":
                errors.append(f"{where}: field {name!r}: lead applies only to rmask")
            if f.zero_as is not None and f.kind != "uimm":
                errors.append(f"{where}: field {name!r}: zero_as applies only to uimm")
            if f.asm_range and (f.kind not in NUMERIC or len(f.asm_range) != 2):
                errors.append(f"{where}: field {name!r}: asm_range is [lo, hi], for uimm/simm only")
            if f.kind != "enum" and f.values:
                errors.append(f"{where}: field {name!r}: values apply only to enum")
            if (f.scale != 1 or f.add) and f.kind not in NUMERIC:
                errors.append(f"{where}: field {name!r}: scale and add apply only to {', '.join(NUMERIC)}")
        if not isinstance(i.asm_no_syntax, bool):
            errors.append(f"{where}: asm_no_syntax must be a boolean")
        if i.asm_no_syntax and (i.asm_syntax or i.asm_wrong or i.asm_rejects):
            errors.append(f"{where}: asm_no_syntax cannot be combined with asm_syntax, asm_wrong or asm_rejects")
        names = placeholders(i.syntax)
        for name in set(names) - i.fields.keys():
            errors.append(f"{where}: syntax uses {{{name}}}, which is not a field")
        for name in i.fields.keys() - set(names) - set(i.block.values()):
            errors.append(f"{where}: field {name!r} never appears in syntax")
        if i.block and (set(i.block) - {"then", "else"} or any(
                f not in i.fields or i.fields[f].kind != "uimm" for f in i.block.values())):
            errors.append(f"{where}: block is {{then = field, else = field}}, both uimm fields")
        if SYNTAX_TOKEN.sub("", i.syntax).count("{") or SYNTAX_TOKEN.sub("", i.syntax).count("}"):
            errors.append(f"{where}: syntax has a lone brace; write {{{{ or }}}} for a literal one")
        for name in set(placeholders(i.asm_syntax)) - i.fields.keys():
            errors.append(f"{where}: asm_syntax uses {{{name}}}, which is not a field")
        if i.body and (i.body not in i.fields or i.fields[i.body].kind not in NUMERIC):
            errors.append(f"{where}: body {i.body!r} is not a uimm or simm field")
        for cpu in i.absent_on:
            if cpu not in CPU_MODELS:
                errors.append(f"{where}: unknown CPU model {cpu!r} in absent_on")
        for chip, what in i.hw.items():
            if chip not in CHIPS:
                errors.append(f"{where}: unknown chip {chip!r} in hw")
            if what.startswith("as:"):
                if what[3:] not in ids or what[3:] == i.id:
                    errors.append(f"{where}: hw {chip} = {what!r} names no other entry")
            elif what not in HW_KINDS:
                errors.append(f"{where}: hw {chip} = {what!r} is not one of {sorted(HW_KINDS)} or as:<id>")
            if "hardware" not in i.sources:
                errors.append(f"{where}: hw needs the source 'hardware'")
        if i.status not in STATUSES:
            errors.append(f"{where}: unknown status {i.status!r}")
        for s in i.sources:
            if s not in sources:
                errors.append(f"{where}: unknown source {s!r} (not in docs/SOURCES.md)")
        if i.status == "guessed" and not i.notes:
            errors.append(f"{where}: guessed, but no notes on how")
        if i.status == "documented" and not [s for s in i.sources if s != "oracle"]:
            errors.append(f"{where}: documented, but no source other than the oracle")
        if i.status == "confirmed":
            observed = [x for x in i.examples if x.origin.startswith("oracle:")]
            if "oracle" not in i.sources or not observed:
                errors.append(f"{where}: confirmed, but no oracle source and observed examples")
            for letter in i.fields:
                values = {codec.fields_of(i, x.bytes).get(letter) for x in observed}
                if len(values) < 2:
                    errors.append(f"{where}: confirmed, but field {letter!r} is seen at fewer than 2 values")
    for n, a in enumerate(table.insns):
        for b in table.insns[n + 1:]:
            if a.words and b.words and _overlap(a, b):
                errors.append(f"{a.id} and {b.id}: encodings overlap")
    if errors:
        return errors       # examples are only meaningful in a consistent table
    for i in table.insns:
        for x in i.examples:
            got = codec.decode(table, x.bytes)
            if got is None or got[0] is not i:
                errors.append(f"{i.id}: example {x.bytes.hex(' ')} decodes to {got[0].id if got else 'nothing'}")
            elif got[2] != len(x.bytes):
                errors.append(f"{i.id}: example {x.bytes.hex(' ')} is {len(x.bytes)} bytes, the entry {got[2]}")
            elif codec.format(i, got[1]) != x.text:
                errors.append(f"{i.id}: example {x.bytes.hex(' ')} prints {codec.format(i, got[1])!r}, "
                              f"the example says {x.text!r}")
    return errors


def hw_note(insn, chip):
    """What `insn` does on `chip` when that is not what its syntax says ("resets the chip", "runs as add4..."), or
    None."""
    what = insn.hw.get(chip)
    if what is None:
        return None
    return f"runs as {what[3:]}" if what.startswith("as:") else HW_KINDS[what]


def shown_by_objdump(insn, cpu="generic", features=()):
    """Whether JieLi's objdump with -mcpu=cpu (or none: "generic") and -mattr=features shows this form."""
    have = set(features) | set(CPU_FEATURES.get(cpu, ()))
    return cpu not in insn.absent_on and set(insn.decode_requires) <= have
