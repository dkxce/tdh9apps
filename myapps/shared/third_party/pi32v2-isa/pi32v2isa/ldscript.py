# SPDX-FileCopyrightText: 2026 Julius Vitkauskas
# SPDX-License-Identifier: MIT-0
"""Linker-script parser for pi32v2isa.ld (docs/LINKER.md, section 2): the subset the FM-1 projects use, plus a
little more of the documented language. It produces plain data (dataclasses) and expression trees; the linker
(ld.py) evaluates them."""

import re
from dataclasses import dataclass, field


class ScriptError(Exception):
    pass


MASK64 = (1 << 64) - 1

# --- AST ------------------------------------------------------------------------------------------------------


@dataclass
class Assign:
    name: str            # symbol name, or "." for the location counter
    op: str              # "=", "+=", ...
    expr: object
    provide: bool = False
    hidden: bool = False


@dataclass
class InputSpec:
    file_pats: list            # file name patterns (None: any); EXCLUDE_FILE before the file name goes in file_excl
    file_excl: list
    sections: list             # [(pattern, [excluded file patterns])]
    keep: bool = False
    sort: str = None           # None, "name", "alignment" (applied to the sections the statement matches)


@dataclass
class DataStmt:
    kind: str            # BYTE SHORT LONG QUAD SQUAD
    expr: object


@dataclass
class FillStmt:
    expr: object


@dataclass
class AssertStmt:
    expr: object
    message: str


@dataclass
class OutSec:
    name: str
    addr: object = None
    type: str = None          # NOLOAD, ...
    at: object = None
    align: object = None
    subalign: object = None
    stmts: list = field(default_factory=list)
    region: str = None
    lma_region: str = None
    phdrs: list = field(default_factory=list)
    fill: object = None


@dataclass
class Region:
    name: str
    attrs: str
    origin: object
    length: object


@dataclass
class Script:
    entry: str = None
    regions: list = field(default_factory=list)
    sections: list = None         # SECTIONS statements (Assign, OutSec, AssertStmt) or None if no SECTIONS command
    top: list = field(default_factory=list)       # assignments and asserts outside SECTIONS, in order
    extern: list = field(default_factory=list)
    search_dirs: list = field(default_factory=list)
    inputs: list = field(default_factory=list)


# --- lexer ----------------------------------------------------------------------------------------------------

NUMBER = re.compile(r"0[xX][0-9a-fA-F]+[KkMm]?|[0-9]+[KkMm]?")
IDENT = re.compile(r"[A-Za-z_.$][A-Za-z0-9_.$]*")
WILD = re.compile(r"[A-Za-z0-9_.$*?\[\]/\\~^!:+@-]+")
OPS = ["<<=", ">>=", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "&=", "|=", "^=", "%="]
SINGLE = set("+-*/%&|^~!<>=?:(){},;")
ASSIGN_OPS = ("=", "+=", "-=", "*=", "/=", "<<=", ">>=", "&=", "|=", "^=", "%=")


def parse_number(text):
    mult = 1
    if text[-1] in "KkMm" and not (text[:2].lower() == "0x" and len(text) == 3 and False):
        # "0x10K": suffix; but 0x1c is plain hex (c is a digit), K and M are not hex digits
        mult = 1024 if text[-1] in "Kk" else 1024 * 1024
        text = text[:-1]
    if text[:2].lower() == "0x":
        return int(text, 16) * mult
    if len(text) > 1 and text[0] == "0" and text.isdigit() and all(c in "01234567" for c in text):
        return int(text, 8) * mult
    return int(text, 10) * mult


class Lexer:
    def __init__(self, text, name="<script>"):
        self.text, self.pos, self.name = text, 0, name

    def error(self, msg):
        line = self.text.count("\n", 0, self.pos) + 1
        raise ScriptError(f"{self.name}:{line}: {msg}")

    def skip(self):
        t, n = self.text, len(self.text)
        while self.pos < n:
            c = t[self.pos]
            if c.isspace():
                self.pos += 1
            elif t.startswith("/*", self.pos):
                end = t.find("*/", self.pos + 2)
                if end < 0:
                    self.error("unterminated comment")
                self.pos = end + 2
            elif c == "#":
                end = t.find("\n", self.pos)      # a stray cpp line marker or comment
                self.pos = n if end < 0 else end
            else:
                break

    def scan(self, wild=False):
        """(kind, text, end position) of the next token without consuming it."""
        self.skip()
        t, p = self.text, self.pos
        if p >= len(t):
            return ("eof", "", p)
        c = t[p]
        if c == '"':
            end = t.find('"', p + 1)
            if end < 0:
                self.error("unterminated string")
            return ("str", t[p + 1:end], end + 1)
        if t.startswith("/DISCARD/", p):
            return ("word", "/DISCARD/", p + 9)
        if wild:
            m = WILD.match(t, p)
            if m:
                return ("word", m.group(), m.end())
        if c.isdigit():
            m = NUMBER.match(t, p)
            # a number directly followed by identifier characters is a name (e.g. 3dnow)
            nxt = t[m.end():m.end() + 1]
            if not (nxt.isalnum() or nxt == "_"):
                return ("num", m.group(), m.end())
        m = IDENT.match(t, p)
        if m:
            return ("word", m.group(), m.end())
        for op in OPS:
            if t.startswith(op, p):
                return ("op", op, p + len(op))
        if c in SINGLE:
            return ("op", c, p + 1)
        if c in "[]\\":
            return ("word", c, p + 1)
        self.error(f"unexpected character {c!r}")

    def peek(self, wild=False):
        k, s, _ = self.scan(wild)
        return k, s

    def next(self, wild=False):
        k, s, end = self.scan(wild)
        self.pos = end
        return k, s

    def accept(self, text, wild=False):
        k, s, end = self.scan(wild)
        if k in ("op", "word") and s == text:
            self.pos = end
            return True
        return False

    def expect(self, text, wild=False):
        if not self.accept(text, wild):
            k, s = self.peek(wild)
            self.error(f"expected {text!r}, found {s or 'end of file'!r}")

    def word(self, wild=False):
        k, s = self.next(wild)
        if k not in ("word", "str"):
            self.error(f"expected a name, found {s or 'end of file'!r}")
        return s


# --- expressions ----------------------------------------------------------------------------------------------

BINARY = [["||"], ["&&"], ["|"], ["^"], ["&"], ["==", "!="], ["<", ">", "<=", ">="], ["<<", ">>"], ["+", "-"],
          ["*", "/", "%"]]
FUNCTIONS = {"ABSOLUTE", "ADDR", "ALIGN", "ALIGNOF", "BLOCK", "DEFINED", "LENGTH", "LEN", "LOADADDR", "LOG2CEIL",
             "MAX", "MIN", "NEXT", "ORIGIN", "ORG", "SEGMENT_START", "SIZEOF", "SIZEOF_HEADERS", "CONSTANT",
             "DATA_SEGMENT_ALIGN", "DATA_SEGMENT_END", "DATA_SEGMENT_RELRO_END", "ASSERT"}


def parse_expr(lx, level=-1):
    """Expression with C precedence; ?: lowest. Returns a tree of tuples."""
    if level == -1:
        cond = parse_expr(lx, 0)
        if lx.peek() == ("op", "?"):
            lx.next()
            a = parse_expr(lx, -1)
            lx.expect(":")
            b = parse_expr(lx, -1)
            return ("cond", cond, a, b)
        return cond
    if level >= len(BINARY):
        return parse_unary(lx)
    left = parse_expr(lx, level + 1)
    while True:
        k, s = lx.peek()
        if k == "op" and s in BINARY[level]:
            lx.next()
            left = ("bin", s, left, parse_expr(lx, level + 1))
        else:
            return left


def parse_unary(lx):
    k, s = lx.peek()
    if k == "op" and s in ("-", "!", "~", "+"):
        lx.next()
        return ("un", s, parse_unary(lx))
    return parse_primary(lx)


def parse_primary(lx):
    k, s = lx.next()
    if k == "num":
        return ("num", parse_number(s), s)
    if k == "op" and s == "(":
        e = parse_expr(lx)
        lx.expect(")")
        return e
    if k == "str":
        return ("name", s)
    if k == "word":
        if s == ".":
            return ("dot",)
        if s in FUNCTIONS and lx.peek() == ("op", "("):
            lx.next()
            args = []
            if s in ("ADDR", "ALIGNOF", "LOADADDR", "SIZEOF", "ORIGIN", "ORG", "LENGTH", "LEN", "DEFINED", "CONSTANT"):
                args.append(("name", lx.word(wild=True)))       # a section, region or symbol name
            elif not lx.accept(")"):
                args.append(parse_expr(lx))
                while lx.accept(","):
                    args.append(parse_expr(lx))
                lx.expect(")")
                return ("call", s, args)
            lx.expect(")")
            return ("call", s, args)
        if s == "SIZEOF_HEADERS":
            return ("call", s, [])
        return ("name", s)
    lx.error(f"unexpected {s or 'end of file'!r} in expression")


# --- statements -----------------------------------------------------------------------------------------------

SORT_WORDS = {"SORT": "name", "SORT_BY_NAME": "name", "SORT_BY_ALIGNMENT": "alignment",
              "SORT_NONE": None, "SORT_BY_INIT_PRIORITY": "name"}
DATA_WORDS = ("BYTE", "SHORT", "LONG", "QUAD", "SQUAD")


def parse_assignment(lx, name):
    """After the target name: op expr ';'. Returns Assign."""
    k, op = lx.next()
    if op not in ASSIGN_OPS:
        lx.error(f"expected an assignment operator after {name!r}")
    e = parse_expr(lx)
    lx.accept(";") or lx.accept(",")
    return Assign(name, op, e)


def parse_provide(lx, word):
    lx.expect("(")
    name = lx.word()
    lx.expect("=")
    e = parse_expr(lx)
    lx.expect(")")
    lx.accept(";")
    return Assign(name, "=", e, provide=word in ("PROVIDE", "PROVIDE_HIDDEN"),
                  hidden=word in ("PROVIDE_HIDDEN", "HIDDEN"))


def parse_assert(lx):
    lx.expect("(")
    e = parse_expr(lx)
    msg = "assertion failed"
    if lx.accept(","):
        k, msg = lx.next()
    lx.expect(")")
    lx.accept(";")
    return AssertStmt(e, msg)


def parse_input_spec(lx, keep=False, sort=None):
    """[EXCLUDE_FILE(files)] file-pattern ( [EXCLUDE_FILE(files)] section-pattern ... ), also wrapped in SORT*()."""
    file_excl = []
    word = lx.word(True)
    if word == "EXCLUDE_FILE":
        lx.expect("(")
        while not lx.accept(")", True):
            file_excl.append(lx.word(True))
        word = lx.word(True)
    if word in SORT_WORDS and lx.peek() == ("op", "("):
        lx.next()
        inner = parse_input_spec(lx, keep, SORT_WORDS[word])
        lx.expect(")")
        return inner
    spec = InputSpec([None if word == "*" else word], file_excl, [], keep, sort)
    lx.expect("(", True)
    pending = []
    while True:
        k, w = lx.peek(True)
        if k == "eof":
            lx.error("unterminated input section list")
        lx.next(True)
        if k == "op" and w == ")":
            return spec
        if k == "op" and w == ",":
            continue
        if w == "EXCLUDE_FILE":
            lx.expect("(")
            while not lx.accept(")", True):
                pending.append(lx.word(True))
            continue
        if w in SORT_WORDS and lx.peek() == ("op", "("):
            lx.next()
            spec.sort = SORT_WORDS[w]
            while not lx.accept(")", True):
                spec.sections.append((lx.word(True), pending))
                pending = []
            continue
        spec.sections.append((w, pending))
        pending = []


def parse_section_body(lx):
    """Statements between the braces of an output section."""
    stmts = []
    while True:
        k, s = lx.peek(True)
        if k == "op" and s == "}":
            lx.next()
            return stmts
        if k == "eof":
            lx.error("unterminated output section")
        if s == ";":
            lx.next()
            continue
        if s == "KEEP":
            lx.next(True)
            lx.expect("(")
            stmts.append(parse_input_spec(lx, keep=True))
            lx.expect(")")
            continue
        if s in DATA_WORDS:
            lx.next(True)
            lx.expect("(")
            e = parse_expr(lx)
            lx.expect(")")
            lx.accept(";")
            stmts.append(DataStmt(s, e))
            continue
        if s == "FILL":
            lx.next(True)
            lx.expect("(")
            e = parse_expr(lx)
            lx.expect(")")
            lx.accept(";")
            stmts.append(FillStmt(e))
            continue
        if s == "ASSERT":
            lx.next(True)
            stmts.append(parse_assert(lx))
            continue
        if s in ("PROVIDE", "PROVIDE_HIDDEN", "HIDDEN"):
            lx.next(True)
            stmts.append(parse_provide(lx, s))
            continue
        if s in ("CREATE_OBJECT_SYMBOLS", "CONSTRUCTORS"):
            lx.error(f"{s} is not supported")
        # assignment or input-section statement
        save = lx.pos
        lx.next(True)
        name = s
        trailing = ""
        if len(name) > 1 and name[-1] in "+-*/&|^%<>" and lx.peek()[1] == "=":
            name, trailing = name[:-1], name[-1]
        k3, s3 = lx.peek()
        if IDENT.fullmatch(name) and k3 == "op" and (s3 in ASSIGN_OPS or trailing):
            if trailing:
                lx.next()
                op = trailing + "="
                e = parse_expr(lx)
                lx.accept(";") or lx.accept(",")
                stmts.append(Assign(name, op, e))
            else:
                stmts.append(parse_assignment(lx, name))
            continue
        lx.pos = save
        stmts.append(parse_input_spec(lx))


def parse_output_section(lx, name):
    sec = OutSec(name)
    # optional address, optional (type), then ':'
    k, s = lx.peek()
    if not (k == "op" and s == ":"):
        if k == "op" and s == "(":
            save = lx.pos
            lx.next()
            k2, w = lx.peek()
            if w in ("NOLOAD", "DSECT", "COPY", "INFO", "OVERLAY"):
                lx.next()
                lx.expect(")")
                sec.type = w
            else:
                lx.pos = save
                sec.addr = parse_expr(lx)
        else:
            sec.addr = parse_expr(lx)
            k, s = lx.peek()
            if k == "op" and s == "(":
                lx.next()
                sec.type = lx.word()
                lx.expect(")")
    lx.expect(":")
    while True:
        k, s = lx.peek()
        if k == "word" and s == "AT" and False:
            pass
        if k == "word" and s in ("AT", "ALIGN", "SUBALIGN", "ONLY_IF_RO", "ONLY_IF_RW", "ALIGN_WITH_INPUT") and \
                lx.peek()[1] != "{":
            lx.next()
            if s in ("ONLY_IF_RO", "ONLY_IF_RW", "ALIGN_WITH_INPUT"):
                continue
            lx.expect("(")
            e = parse_expr(lx)
            lx.expect(")")
            if s == "AT":
                sec.at = e
            elif s == "ALIGN":
                sec.align = e
            else:
                sec.subalign = e
            continue
        break
    lx.expect("{")
    sec.stmts = parse_section_body(lx)
    # trailer: > region, AT > region, :phdr, =fill
    while True:
        k, s = lx.peek()
        if k == "op" and s == ">":
            lx.next()
            sec.region = lx.word()
        elif k == "word" and s == "AT":
            lx.next()
            lx.expect(">")
            sec.lma_region = lx.word()
        elif k == "op" and s == ":":
            lx.next()
            sec.phdrs.append(lx.word())
        elif k == "op" and s == "=":
            lx.next()
            sec.fill = parse_expr(lx)
        elif k == "op" and s == ",":
            lx.next()
        else:
            break
    return sec


def parse_sections(lx):
    lx.expect("{")
    stmts = []
    while True:
        k, s = lx.peek(True)
        if k == "op" and s == "}":
            lx.next()
            return stmts
        if k == "eof":
            lx.error("unterminated SECTIONS")
        if s == ";":
            lx.next()
            continue
        if s == "ENTRY":
            lx.next(True)
            lx.expect("(")
            stmts.append(("entry", lx.word()))
            lx.expect(")")
            continue
        if s == "ASSERT":
            lx.next(True)
            stmts.append(parse_assert(lx))
            continue
        if s in ("PROVIDE", "PROVIDE_HIDDEN", "HIDDEN"):
            lx.next(True)
            stmts.append(parse_provide(lx, s))
            continue
        if s == "/DISCARD/":
            lx.next(True)
            lx.expect(":")
            lx.expect("{")
            sec = OutSec("/DISCARD/")
            sec.stmts = parse_section_body(lx)
            stmts.append(sec)
            continue
        if s == "INCLUDE":
            lx.error("INCLUDE is not supported")
        # symbol assignment or output section: both start with a name
        save = lx.pos
        lx.next(True)
        name = s
        trailing = ""
        if len(name) > 1 and name[-1] in "+-*/&|^%<>" and lx.peek()[1] == "=":
            name, trailing = name[:-1], name[-1]
        k3, s3 = lx.peek()
        if k3 == "op" and (s3 in ASSIGN_OPS or trailing) and name:
            if trailing:
                lx.next()
                e = parse_expr(lx)
                lx.accept(";") or lx.accept(",")
                stmts.append(Assign(name, trailing + "=", e))
            else:
                stmts.append(parse_assignment(lx, name))
            continue
        stmts.append(parse_output_section(lx, name))


def parse_memory(lx):
    lx.expect("{")
    regions = []
    while not lx.accept("}"):
        name = lx.word()
        attrs = ""
        if lx.accept("("):
            while not lx.accept(")"):
                attrs += lx.next(True)[1]
        lx.expect(":")
        vals = {}
        for _ in range(2):
            key = lx.word().upper()
            lx.expect("=")
            vals[key] = parse_expr(lx)
            lx.accept(",")
        origin = vals.get("ORIGIN") or vals.get("ORG") or vals.get("O")
        length = vals.get("LENGTH") or vals.get("LEN") or vals.get("L")
        if origin is None or length is None:
            lx.error(f"region {name}: ORIGIN and LENGTH are needed")
        regions.append(Region(name, attrs, origin, length))
    return regions


def parse_script(text, name="<script>"):
    lx = Lexer(text, name)
    sc = Script()
    while True:
        k, s = lx.peek(True)
        if k == "eof":
            return sc
        if k == "op" and s == ";":
            lx.next()
            continue
        lx.next(True)
        if s == "ENTRY":
            lx.expect("(")
            sc.entry = lx.word()
            lx.expect(")")
        elif s == "MEMORY":
            sc.regions += parse_memory(lx)
        elif s == "SECTIONS":
            sc.sections = (sc.sections or []) + parse_sections(lx)
        elif s in ("OUTPUT_FORMAT", "OUTPUT_ARCH", "TARGET", "STARTUP", "OUTPUT", "NOCROSSREFS", "FORCE_COMMON_ALLOCATION",
                   "LD_FEATURE", "SEARCH_DIR"):
            if lx.accept("("):
                while not lx.accept(")"):
                    k2, w = lx.next(True)
                    if k2 == "eof":
                        lx.error("unterminated command")
                    if s == "SEARCH_DIR" and k2 in ("word", "str"):
                        sc.search_dirs.append(w)
        elif s in ("INPUT", "GROUP"):
            lx.expect("(")
            while not lx.accept(")"):
                k2, w = lx.next(True)
                if k2 == "eof":
                    lx.error("unterminated command")
                if w != ",":
                    sc.inputs.append(w)
        elif s == "EXTERN":
            lx.expect("(")
            while not lx.accept(")"):
                sc.extern.append(lx.word(True))
        elif s == "ASSERT":
            sc.top.append(parse_assert(lx))
        elif s in ("PROVIDE", "PROVIDE_HIDDEN", "HIDDEN"):
            sc.top.append(parse_provide(lx, s))
        elif s in ("PHDRS", "VERSION"):
            lx.error(f"{s} is not supported")
        elif IDENT.fullmatch(s):
            sc.top.append(parse_assignment(lx, s))
        else:
            lx.error(f"unexpected {s!r}")
