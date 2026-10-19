#!/usr/bin/env python3
"""Find the a*b + c the GameCube fused, and write them as it computes them.

mwcc compiled most a*b + c in Melee into one fused instruction (fmadds,
fmsubs, fnmadds, fnmsubs; fmadd and the rest in double), rounded once; our
build never contracts (-ffp-contract=off), so it rounds twice and comes out
an ULP apart, which a Slippi peer in Dolphin does not (tools/slp_compare.py).
This tool counts each function's fused instructions in the disc's DOL, finds
the candidate expressions in the function's C (libclang, with the build's
own compile command), and rewrites them with src/pc/gekko_fp.h's helpers --
but only in a function whose candidates are exactly as many as the DOL's
fused instructions, of each precision. The rest are listed for a person.

    tools/fuse_sites.py --iso DISC.iso --symbols symbols.txt FILE.c ... [--apply]

symbols.txt is the decomp's config/GALE01/symbols.txt (doldecomp/melee).
Needs the libclang Python package (pip install libclang) and
build/linux/compile_commands.json. Without --apply it only reports.

The rewrite follows what the DOL shows mwcc doing:
  a*b + c, c + a*b    fmadds(a, b, c)
  a*b + c*d           fmadds(a, b, c*d)    the first product fused -- but not
                                           x*x + y*y of values read from memory
                                           (members, elements, a local struct whose
                                           address is taken), nor a third square
                                           added on to them
  a*b - c             fmsubs(a, b, c)      (a*b - c*d likewise)
  c - a*b             fnmsubs(a, b, c)
  -(a*b) + c          fnmsubs(a, b, c)     (c + -(a*b) likewise)
  -(a*b + c)          fnmadds(a, b, c)
  x += a*b            x = fmadds(a, b, x)
  x -= a*b            x = fnmsubs(a, b, x)
in float (the s forms) or double, never across a change of precision. A
product written twice with nothing it reads changed in between (2.0F * t)
mwcc computes once and does not fuse.

FUSE_SHOW=<function> lists that function's sites (and its inlined helpers').
"""
import argparse
import collections
import ctypes
import json
import os
import re
import shlex
import struct
import sys

import clang.cindex as ci

# LIBCLANG=<path to libclang.so>: a system libclang instead of the
# package's own (its builtin headers then come with it).
if os.environ.get("LIBCLANG"):
    ci.Config.set_library_file(os.environ["LIBCLANG"])

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# ---- the DOL ---------------------------------------------------------------

FUSED = {28: "msub", 29: "madd", 30: "nmsub", 31: "nmadd"}  # A-form xo


def dol_reader(iso):
    f = open(iso, "rb")
    f.seek(0x420)
    dol = struct.unpack(">I", f.read(4))[0]
    f.seek(dol)
    h = f.read(0x100)
    offs = struct.unpack(">18I", h[0:0x48])
    addrs = struct.unpack(">18I", h[0x48:0x90])
    sizes = struct.unpack(">18I", h[0x90:0xD8])

    def read(a, n):
        for o, s, z in zip(offs, addrs, sizes):
            if s <= a < s + z:
                f.seek(dol + o + a - s)
                return f.read(n)
        return b""
    return read


def census(iso, symbols):
    """name -> Counter of 'single'/'double' fused instructions; and name ->
    (address, the addresses it calls)."""
    read = dol_reader(iso)
    out = {}
    calls = {}
    weak = {}  # address -> name: out-of-line copies of inline functions (sqrtf__Ff)
    for line in open(symbols):
        m = re.match(r"(\S+) = \.text:0x([0-9A-F]+); // type:function size:0x([0-9A-F]+)", line)
        if not m:
            continue
        name, addr, size = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
        if "scope:weak" in line:
            weak[addr] = name
        code = read(addr, size)
        c = collections.Counter()
        targets = collections.Counter()
        for i in range(0, len(code) - 3, 4):
            w = struct.unpack(">I", code[i:i + 4])[0]
            op, xo = w >> 26, (w >> 1) & 31
            if op == 59 and xo in FUSED:
                c["single"] += 1
            elif op == 63 and xo in FUSED:
                c["double"] += 1
            elif op == 18 and (w & 2) == 0:  # bl, or b out of the function (a tail call)
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                t = (addr + i + li) & 0xFFFFFFFF
                if (w & 1) or not addr <= t < addr + size:
                    targets[t] += 1
        out.setdefault(name, collections.Counter()).update(c)
        calls[name] = (addr, targets)
    # A weak function is an inline one mwcc emitted out of line and called;
    # the C inlines it, and counts its fused ops in the caller, once a call.
    own = {n: collections.Counter(c) for n, c in out.items()}
    for name, (addr, targets) in calls.items():
        for t, n in targets.items():
            if t in weak and weak[t] != name:
                for _ in range(n):
                    out[name].update(own.get(weak[t], {}))
    return out, calls


# ---- the C -------------------------------------------------------------------

def compile_args(db, path):
    for e in db:
        if os.path.realpath(os.path.join(e["directory"], e["file"])) == os.path.realpath(path):
            args = e.get("arguments") or shlex.split(e["command"])
            out, skip = [], False
            for a in args[1:]:
                if skip:
                    skip = False
                    continue
                if a in ("-o", "-c", "-MF", "-MT", "-MQ"):
                    skip = a != "-c"
                    continue
                if a == path or a.endswith(os.path.basename(path)) or a.startswith("-M"):
                    continue
                if a.startswith(("-fexec-charset", "-finput-charset")):
                    continue  # GCC's; clang has only UTF-8, and it changes no expression
                out.append(a)
            return out, e["directory"]
    return None, None


def strip(c):
    """Through parentheses and casts that keep the type."""
    while True:
        kids = list(c.get_children())
        if c.kind == ci.CursorKind.PAREN_EXPR and len(kids) == 1:
            c = kids[0]
        elif c.kind == ci.CursorKind.UNEXPOSED_EXPR and len(kids) == 1 and \
                kids[0].type.get_canonical().spelling == c.type.get_canonical().spelling:
            c = kids[0]
        else:
            return c


def fp(c):
    s = c.type.get_canonical().spelling
    return "single" if s == "float" else "double" if s == "double" else None


# libclang 17 on has the operators' kinds (the Python bindings may not):
# inside a macro (SQ(x)) the operands' extents are the whole expansion, so
# the text between them says nothing.
_BINOPS = ["", ".*", "->*", "*", "/", "%", "+", "-", "<<", ">>", "<=>", "<", ">", "<=", ">=", "==", "!=",
           "&", "^", "|", "&&", "||", "=", "*=", "/=", "%=", "+=", "-=", "<<=", ">>=", "&=", "^=", "|=", ","]
_UNOPS = ["", "++", "--", "++", "--", "&", "*", "+", "-", "~", "!"]
_binop_kind = _unop_kind = None


def _op_kinds():
    global _binop_kind, _unop_kind
    if _binop_kind is None:
        try:
            _binop_kind = ci.conf.lib.clang_getCursorBinaryOperatorKind
            _binop_kind.argtypes, _binop_kind.restype = [ci.Cursor], ctypes.c_int
            _unop_kind = ci.conf.lib.clang_getCursorUnaryOperatorKind
            _unop_kind.argtypes, _unop_kind.restype = [ci.Cursor], ctypes.c_int
        except AttributeError:
            _binop_kind = _unop_kind = False
    return _binop_kind, _unop_kind


def op_of(c, src):
    """A binary or compound operator's spelling."""
    kids = list(c.get_children())
    if len(kids) != 2:
        return None
    binop, _ = _op_kinds()
    if binop and c.kind in (ci.CursorKind.BINARY_OPERATOR, ci.CursorKind.COMPOUND_ASSIGNMENT_OPERATOR):
        k = binop(c)
        return _BINOPS[k] if 0 < k < len(_BINOPS) else None
    a, b = kids
    if a.extent.end.offset > b.extent.start.offset:
        return None
    return src[a.extent.end.offset:b.extent.start.offset].strip() or None


def unop_of(c, src):
    """A unary operator's spelling."""
    _, unop = _op_kinds()
    if unop:
        k = unop(c)
        return _UNOPS[k] if 0 < k < len(_UNOPS) else None
    kids = list(c.get_children())
    return src[c.extent.start.offset:kids[0].extent.start.offset].strip() if kids else None


def _locals_only(m):
    """A product of plain variables (no member, element or call)."""
    for k in strip(m).get_children():
        k = strip(k)
        if k.kind != ci.CursorKind.DECL_REF_EXPR:
            return False
    return True


def _local_member(k, src):
    """s.x of a local struct (not through a pointer): mwcc keeps those in
    registers (d1 in lbColl_80005C44, 0x80005DDC)."""
    if k.kind != ci.CursorKind.MEMBER_REF_EXPR or "->" in src[k.extent.start.offset:k.extent.end.offset]:
        return False
    base = [strip(c) for c in k.get_children()]
    if not (len(base) == 1 and base[0].kind == ci.CursorKind.DECL_REF_EXPR and
            base[0].referenced is not None and base[0].referenced.kind == ci.CursorKind.VAR_DECL and
            base[0].referenced.semantic_parent.kind == ci.CursorKind.FUNCTION_DECL):
        return False
    # used whole anywhere (&s, s = *p, passed by value): it lives on the stack
    var = base[0].referenced
    fn = var.semantic_parent
    key = (fn.hash, var.hash)
    if key not in _whole_use:
        whole = False

        def walk(n, parent):
            nonlocal whole
            if n.kind == ci.CursorKind.DECL_REF_EXPR and n.referenced is not None and \
                    n.referenced.hash == var.hash and (parent is None or parent.kind != ci.CursorKind.MEMBER_REF_EXPR):
                whole = True
            if n.kind == ci.CursorKind.VAR_DECL and n.hash == var.hash and \
                    any(c.kind != ci.CursorKind.TYPE_REF for c in n.get_children()):
                whole = True  # initialised whole
            for ch in n.get_children():
                walk(ch, n if n.kind != ci.CursorKind.UNEXPOSED_EXPR else parent)
        walk(fn, None)
        _whole_use[key] = whole
    return not _whole_use[key]


_whole_use = {}


def _mem_square(m, src):
    """x*x of a value read from memory (a member, element or call)."""
    if _locals_only(m):
        return False
    if all(_local_member(strip(k), src) for k in strip(m).get_children()):
        return False
    a, c = [strip(k) for k in strip(m).get_children()]
    text = lambda k: re.sub(r"\s", "", src[k.extent.start.offset:k.extent.end.offset])
    return text(a) == text(c)


def _sum_of_products(n, src, prec):
    """a*b + c*d (the left of a longer sum of products)."""
    n = strip(n)
    if n.kind != ci.CursorKind.BINARY_OPERATOR or op_of(n, src) not in ("+", "-"):
        return False
    l, r = list(n.get_children())
    return is_mul(l, src, prec) and is_mul(r, src, prec)


def _overlap(a, b):
    return a.extent.end.offset > b.extent.start.offset


def mul_operands(m, src):
    """A product's two operands to write out: cursors, or text where it came
    from SQ(x); None where a macro hides them."""
    a, c = list(strip(m).get_children())
    if (a.extent.start.offset, a.extent.end.offset) != (c.extent.start.offset, c.extent.end.offset):
        return a, c
    text = src[a.extent.start.offset:a.extent.end.offset].strip()
    t = re.fullmatch(r"SQ\((.*)\)", text, re.S)
    if t:
        return f"({t.group(1)})", f"({t.group(1)})"
    # the SDK's degree and radian conversions: (a) * a constant
    t = re.fullmatch(r"MTX(DegToRad|RadToDeg)\((.*)\)", text, re.S)
    if t:
        return f"({t.group(2)})", "0.01745329252f" if t.group(1) == "DegToRad" else "57.29577951f"
    return None


class Site:
    def __init__(self, node, kind, prec, a, c, b, assign=None):
        self.node, self.kind, self.prec, self.a, self.c, self.b, self.assign = node, kind, prec, a, c, b, assign
        self.hidden = False  # inside a macro the source cannot say what to write: counted only


def is_mul(n, src, prec):
    n = strip(n)
    return n.kind == ci.CursorKind.BINARY_OPERATOR and op_of(n, src) == "*" and fp(n) == prec


def neg_mul(n, src, prec):
    """-(a*b): the product, or None."""
    n = strip(n)
    kids = list(n.get_children())
    if n.kind != ci.CursorKind.UNARY_OPERATOR or len(kids) != 1 or unop_of(n, src) != "-":
        return None
    return kids[0] if is_mul(kids[0], src, prec) else None


def _shared_products(fn, src):
    """Offsets of the products mwcc computes once for two uses: the same
    text twice, none of its variables assigned in between (2.0F * t in
    HSD_QuatLib_8037EF28, 0x8037F080)."""
    norm = lambda n: re.sub(r"\s", "", src[n.extent.start.offset:n.extent.end.offset])
    prods = collections.defaultdict(list)  # text -> [(offset, names, reads memory)]
    writes = []  # (offset, name)
    calls = []  # offsets: a call may change what memory holds

    def reads_memory(n):
        for k in n.walk_preorder():
            if k.kind in (ci.CursorKind.MEMBER_REF_EXPR, ci.CursorKind.ARRAY_SUBSCRIPT_EXPR) or \
                    (k.kind == ci.CursorKind.UNARY_OPERATOR and unop_of(k, src) == "*"):
                return True
            if k.kind == ci.CursorKind.DECL_REF_EXPR and k.referenced is not None and \
                    k.referenced.kind == ci.CursorKind.VAR_DECL and \
                    k.referenced.semantic_parent.kind != ci.CursorKind.FUNCTION_DECL:
                return True  # a global
        return False

    def names(n):
        out = set()

        def w(k):
            if k.kind == ci.CursorKind.DECL_REF_EXPR:
                out.add(k.spelling)
            for ch in k.get_children():
                w(ch)
        w(n)
        return out

    def walk(n):
        if n.kind == ci.CursorKind.BINARY_OPERATOR and op_of(n, src) == "*" and fp(n):
            prods[norm(n)].append((n.extent.start.offset, names(n), reads_memory(n)))
        elif n.kind == ci.CursorKind.CALL_EXPR:
            calls.append(n.extent.start.offset)
        elif (n.kind == ci.CursorKind.BINARY_OPERATOR and op_of(n, src) == "=") or \
                n.kind == ci.CursorKind.COMPOUND_ASSIGNMENT_OPERATOR:
            for nm in names(list(n.get_children())[0]):
                writes.append((n.extent.start.offset, nm))
        elif n.kind == ci.CursorKind.UNARY_OPERATOR and unop_of(n, src) in ("++", "--"):
            for nm in names(n):
                writes.append((n.extent.start.offset, nm))
        for ch in n.get_children():
            walk(ch)
    walk(fn)
    shared = set()
    for occ in prods.values():
        for (a, na, mem), (b, _, _) in zip(occ, occ[1:]):
            if not any(a < o < b and nm in na for o, nm in writes) and \
                    not (mem and any(a < o < b for o in calls)):
                shared.update((a, b))
    return shared


def find_sites(fn, src, path):
    sites = []
    shared = _shared_products(fn, src)

    def in_file(n):
        return n.extent.start.file is not None and n.extent.start.file.name == path

    def visit(n, parent):
        if not in_file(n):
            return
        prec = fp(n)
        k = n.kind

        def add(node, kind, mul, b, assign=None):
            if strip(mul).extent.start.offset in shared:
                # computed once for its uses (2.0F * t, 0x8037F080): the DOL
                # adds or subtracts the product it kept
                return
            ops = mul_operands(mul, src)
            # the sum's own operands overlap where a macro wrote it
            hidden = ops is None or (len(list(node.get_children())) == 2 and
                                     op_of(node, src) is not None and
                                     not isinstance(b, str) and
                                     _overlap(*list(node.get_children())))
            a, c = ops if ops else (None, None)
            st = Site(node, kind, prec, a, c, b, assign)
            st.hidden = hidden
            sites.append(st)

        if prec and k == ci.CursorKind.BINARY_OPERATOR and op_of(n, src) in ("+", "-"):
            l, r = list(n.get_children())
            op = op_of(n, src)
            # -(a*b + c): the negation is fused too
            neg = parent is not None and parent.kind == ci.CursorKind.UNARY_OPERATOR and \
                unop_of(parent, src) == "-"
            if is_mul(l, src, prec) and is_mul(r, src, prec) and _mem_square(l, src):
                # x*x + y*y: the DOL fuses the first square where x is a value
                # in a register (two locals, 0x800B0FB8) and not where it comes
                # from memory (the C-stick's squared length, 0x800DF614; the
                # camera's orbit, 0x8002F1CC). A dot product of members it does
                # fuse (lbVector_Angle, 0x8000D748).
                pass
            elif is_mul(l, src, prec) and _mem_square(l, src) and _sum_of_products(r, src, prec):
                # z*z + (x*x + y*y) from memory, the same (lb_8001044C, 0x80010B60)
                pass
            elif is_mul(l, src, prec):
                kind = ("nmadd" if neg else "madd") if op == "+" else "msub"
                add(parent if neg and op == "+" else n, kind, l, r)
            elif is_mul(r, src, prec) and _mem_square(r, src) and _sum_of_products(l, src, prec):
                # (x*x + y*y) + z*z read from memory: the DOL adds the last
                # product on too (fadds), as it does the first two
                pass
            elif is_mul(r, src, prec):
                add(n, "madd" if op == "+" else "nmsub", r, l)
            elif op == "+" and neg_mul(l, src, prec):
                add(n, "nmsub", neg_mul(l, src, prec), r)
            elif op == "+" and neg_mul(r, src, prec):
                add(n, "nmsub", neg_mul(r, src, prec), l)
        elif k == ci.CursorKind.CALL_EXPR and re.match(r"(gk|ppc)_fn?m(add|sub)s?$", n.spelling or ""):
            # already written fused (by hand, or an earlier run): counted, kept,
            # and so are the fused ops in its arguments
            sites.append(Site(n, None, "single" if n.spelling.endswith("s") else "double", None, None, None))
        elif prec and k == ci.CursorKind.COMPOUND_ASSIGNMENT_OPERATOR and op_of(n, src) in ("+=", "-="):
            l, r = list(n.get_children())
            lhs = src[l.extent.start.offset:l.extent.end.offset]
            if is_mul(r, src, prec):
                # x = f(..., x) evaluates x twice: only a plain lvalue
                safe = "(" not in lhs and "++" not in lhs and "--" not in lhs
                add(n, "madd" if op_of(n, src) == "+=" else "nmsub", r, l, assign=l if safe else False)
        for ch in n.get_children():
            visit(ch, n)

    visit(fn, None)
    return sites


HELPER = {("single", k): "gk_f" + k + "s" for k in FUSED.values()}
HELPER.update({("double", k): "gk_f" + k for k in FUSED.values()})


def render(node, src, by_node):
    """The node's source with every site inside it rewritten."""
    s = by_node.get(node.hash)
    if s is not None and s.kind is not None:
        t = lambda c: c if isinstance(c, str) else render(c, src, by_node)
        call = f"{HELPER[(s.prec, s.kind)]}({t(s.a)}, {t(s.c)}, {t(s.b)})"
        return f"{t(s.assign)} = {call}" if s.assign is not None else call
    start, end = node.extent.start.offset, node.extent.end.offset
    out, pos = [], start
    for ch in node.get_children():
        cs, ce = ch.extent.start.offset, ch.extent.end.offset
        if cs < pos or ce > end:
            continue
        out.append(src[pos:cs])
        out.append(render(ch, src, by_node))
        pos = ce
    out.append(src[pos:end])
    return "".join(out)


def site_edits(sites, src):
    """The edits rewriting each outermost site (inner ones are rendered inside it)."""
    by_node = {s.node.hash: s for s in sites}
    inner = set()
    for s in sites:
        for o in sites:
            if o is not s and s.node.extent.start.offset <= o.node.extent.start.offset and \
                    o.node.extent.end.offset <= s.node.extent.end.offset:
                inner.add(o.node.hash)
    return [((s.node.extent.start.offset, s.node.extent.end.offset), render(s.node, src, by_node))
            for s in sites if s.kind is not None and s.node.hash not in inner]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--iso", required=True)
    ap.add_argument("--symbols", required=True)
    ap.add_argument("--db", default=os.path.join(ROOT, "build", "linux", "compile_commands.json"))
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--quiet", action="store_true", help="only the totals and the mismatches")
    args = ap.parse_args()
    dol, dol_calls = census(args.iso, args.symbols)
    db = json.load(open(args.db))
    totals = collections.Counter()
    texts = {}
    pending = collections.defaultdict(dict)  # file -> {(start, end): text}
    # An inline helper's sites are written only if every function that
    # inlines it matched: mwcc fuses per call site, so a helper rewritten for
    # one caller computes the others' wrong.
    helper_edits = collections.defaultdict(list)  # helper usr -> [(file, span, text)]
    veto = set()

    def text(f):
        if f not in texts:
            # latin-1: one character per byte, as libclang's offsets count
            texts[f] = open(f, encoding="latin-1").read()
        return texts[f]

    def inlined_by_mwcc(caller, callee):
        """A function the DOL has, which this caller never branches to:
        mwcc inlined it there (it_802A3C98 in it_802A44CC, 0x802A4530)."""
        if caller not in dol_calls or callee not in dol_calls:
            return False
        return dol_calls[callee][0] not in dol_calls[caller][1]

    for path in [os.path.realpath(p) for p in args.files]:
        cargs, cwd = compile_args(db, path)
        if cargs is None:
            totals["files not built"] += 1
            continue
        os.chdir(cwd)
        tu = ci.Index.create().parse(path, args=cargs)
        errs = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error]
        if errs:
            print(f"{path}: {len(errs)} parse errors, first: {errs[0]}")
            continue
        # Every function this file sees defined. One the DOL has no symbol
        # for (a static inline in a header, a static helper) mwcc inlined:
        # its fused ops are in each caller's count, once per call.
        defs = {}
        for fn in tu.cursor.get_children():
            if fn.kind == ci.CursorKind.FUNCTION_DECL and fn.is_definition() and fn.extent.start.file is not None:
                defs[fn.get_usr()] = fn
        own = {}
        sqrtf_usr = [u for u, d in defs.items() if d.spelling == "gk_sqrtf"]

        def own_sites(usr):
            if usr not in own:
                fn = defs[usr]
                f = os.path.realpath(fn.extent.start.file.name)
                own[usr] = (f, find_sites(fn, text(f), fn.extent.start.file.name))
            return own[usr][1]

        def inlined_calls(usr):
            out = []

            def walk(n):
                if n.kind == ci.CursorKind.CALL_EXPR and n.referenced is not None and \
                        n.referenced.spelling == "gk_sqrtf_call" and sqrtf_usr:
                    # sqrtf__Ff, the inline sqrtf out of line: the DOL's count
                    # takes its fused ops once a call (census), so this does
                    out.append(sqrtf_usr[0])
                elif n.kind == ci.CursorKind.CALL_EXPR and n.referenced is not None:
                    r = n.referenced.get_usr()
                    if r in defs and r != usr and (defs[r].spelling not in dol or
                                                   inlined_by_mwcc(defs[usr].spelling, defs[r].spelling)) and \
                            not re.match(r"(gk|ppc)_fn?m(add|sub)s?$", defs[r].spelling):
                        out.append(r)
                for ch in n.get_children():
                    walk(ch)
            walk(defs[usr])
            return out

        def total(usr, stack=()):
            c = collections.Counter(s.prec for s in own_sites(usr))
            used = [usr]
            if usr in stack:
                return c, used
            for r in inlined_calls(usr):
                cc, uu = total(r, stack + (usr,))
                c.update(cc)
                used += uu
            return c, used

        for usr, fn in defs.items():
            if os.path.realpath(fn.extent.start.file.name) != path:
                continue
            want = dol.get(fn.spelling)
            if want is None:
                totals["not in DOL"] += 1
                continue
            have, used = total(usr)
            if not want and not have:
                continue
            counts = have["single"] == want["single"] and have["double"] == want["double"]
            hidden = any(s.hidden for u in used for s in own_sites(u))
            ok = counts and not hidden and all(s.assign is not False for u in used for s in own_sites(u))
            totals["match" if ok else "in a macro" if counts and hidden else "mismatch"] += 1
            if not args.quiet or not ok:
                print(f"  {fn.spelling}: DOL {want['single']}+{want['double']}d, C {have['single']}+{have['double']}d"
                      f"{' (with ' + str(len(set(used)) - 1) + ' inlined)' if len(set(used)) > 1 else ''}"
                      f" {'ok' if ok else 'IN A MACRO' if counts and hidden else 'MISMATCH'}")
            if os.environ.get("FUSE_SHOW") == fn.spelling:
                for u in used:
                    for st in own_sites(u):
                        print(f"    {defs[u].spelling}:{st.node.extent.start.line} {st.kind} {st.prec}")
            if not ok:
                # its helpers; and itself, which a caller mwcc inlined it
                # into might otherwise rewrite
                veto.update(used)
            else:
                for u in set(used):
                    f = own[u][0]
                    for span, txt in site_edits(own_sites(u), text(f)):
                        if u != usr:
                            helper_edits[u].append((f, span, txt))
                        elif span not in pending[f]:
                            pending[f][span] = txt
                            totals["sites"] += 1
    for u, edits in helper_edits.items():
        if u in veto:
            totals["helper sites held back"] += len(edits)
            continue
        for f, span, txt in edits:
            if span not in pending[f]:
                pending[f][span] = txt
                totals["sites"] += 1
    if args.apply:
        for f, edits in pending.items():
            if not edits:
                continue
            src = text(f)
            for (a, b), txt in sorted(edits.items(), reverse=True):
                src = src[:a] + txt + src[b:]
            if "gekko_fp.h" not in src:
                m = re.search(r"^#include .*$", src, re.M)
                src = src[:m.start()] + "#include <pc/gekko_fp.h>\n" + src[m.start():]
            open(f, "w", encoding="latin-1").write(src)
            print(f"{f}: {len(edits)} rewrites")
    print(dict(totals))


if __name__ == "__main__":
    main()
