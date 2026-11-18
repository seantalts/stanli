#!/usr/bin/env python3
"""Random Stan programs for the O0/O1 differential fuzzer.

generate(seed, config) builds a typed AST whose user functions exercise the
stanc3 inliner and the passes after it. Program.source() prints it as Stan
and Program.data() gives the matching data. The same seed and config always
give the same program.

Container shapes are size symbols: every vector, matrix and array dimension
names an entry of Program.sizes, and literals, data and size expressions
print from the current values. The reducer can then shrink a size and the
whole program follows.

The programs keep six rules so that the unoptimized and optimized programs
can be compared. check_rules() checks them on any AST, generated or reduced:

  assigned    every variable is assigned before it is read
  bounded     loops run at most MAX_TRIPS times, recursion at most
              MAX_RECURSION deep
  finite      only total, smooth, bounded-growth real operations
  one_effect  at most one call with an effect (print, reject, rng, target)
              in a statement's own expressions
  in_range    indexes are in range and array expressions have their
              declared length
  block       rng, target and _lp calls only where Stan permits them

    tools/fuzz_stanc/gen.py SEED [--config FILE] [--data]
"""
from __future__ import annotations

import argparse
import copy
import dataclasses
import json
import random
import sys
from dataclasses import dataclass, field
from typing import List, Optional

MAX_TRIPS = 6
MAX_RECURSION = 4
SIZE_VALUES = (0, 1, 2, 3)

# ---------------------------------------------------------------------------
# Types. Container dimensions are indexes into Program.sizes.


@dataclass(frozen=True)
class T:
    kind: str
    dims: tuple = ()
    elem: Optional["T"] = None
    items: tuple = ()


INT = T("int")
REAL = T("real")


def vec(s):
    return T("vector", (s,))


def rvec(s):
    return T("row_vector", (s,))


def mat(r, c):
    return T("matrix", (r, c))


def arr(s, elem):
    return T("array", (s,), elem)


def tup(*items):
    return T("tuple", (), None, tuple(items))


def unsized(t):
    if t.kind == "array":
        return "array[] " + unsized(t.elem)
    if t.kind == "tuple":
        return "tuple(" + ", ".join(unsized(i) for i in t.items) + ")"
    return t.kind


def decl_syms(t):
    """The size symbols of t in the order a sized declaration prints them."""
    if t.kind in ("vector", "row_vector", "matrix"):
        return list(t.dims)
    if t.kind == "array":
        return [t.dims[0]] + decl_syms(t.elem)
    if t.kind == "tuple":
        return [s for i in t.items for s in decl_syms(i)]
    return []


def sized(t, sizes):
    """t as a sized declaration type; `sizes` iterates the size strings."""
    if t.kind in ("vector", "row_vector"):
        return f"{t.kind}[{next(sizes)}]"
    if t.kind == "matrix":
        return f"matrix[{next(sizes)}, {next(sizes)}]"
    if t.kind == "array":
        n = next(sizes)
        return f"array[{n}] {sized(t.elem, sizes)}"
    if t.kind == "tuple":
        return "tuple(" + ", ".join(sized(i, sizes) for i in t.items) + ")"
    return t.kind


def has_tuple(t):
    return t.kind == "tuple" or (t.elem is not None and has_tuple(t.elem))


# ---------------------------------------------------------------------------
# Expressions.


@dataclass
class Var:
    name: str
    t: T


@dataclass
class Lit:
    """A literal of any type. Containers print with the current sizes;
    element i of a container is v + 0.25 i."""
    t: T
    v: float


@dataclass
class Size:
    """An int expression whose value is Program.sizes[sym]. `how` is lit,
    data (the data variable N<sym>), or rows, cols, size or num_elements of
    `via`, any expression with that dimension (a user call or a variable)."""
    sym: int
    how: str = "lit"
    via: object = None
    t: T = INT


@dataclass
class Op:
    op: str
    a: object
    b: object
    t: T


@dataclass
class Call:
    """A call. fn is the FunDef of a user call and None for Stan's own
    functions. bar prints the first argument before a "|"."""
    name: str
    args: list
    t: T
    fn: object = None
    bar: bool = False


@dataclass
class Div:
    """a / (1 + square(b)); the only division."""
    a: object
    b: object
    t: T


@dataclass
class Cond:
    c: object
    a: object
    b: object
    t: T


@dataclass
class Index:
    """base[idx, ...]. Each index is ("one", int), ("upto", int) for 1:n,
    or ("multi", int array)."""
    base: object
    idx: list
    t: T


@dataclass
class Proj:
    base: object
    k: int
    t: T


@dataclass
class Tup:
    items: list
    t: T


@dataclass
class Clamp:
    """max(1, min(e, hi)): an index that is in range for any int e."""
    e: object
    hi: object
    t: T = INT


@dataclass
class ArrE:
    """An array expression {a, b, ...}. Its length is fixed."""
    items: list
    t: T


EXPR_TYPES = (Var, Lit, Size, Op, Call, Div, Cond, Index, Proj, Tup, Clamp,
              ArrE)

# ---------------------------------------------------------------------------
# Statements. A pinned statement or expression is one the reducer must keep
# (a recursion's base case and its decreasing argument).


@dataclass
class Decl:
    name: str
    t: T
    sizes: list
    init: object
    pinned: bool = False


@dataclass
class Assign:
    lhs: object
    op: str
    rhs: object
    pinned: bool = False


@dataclass
class If:
    cond: object
    then: list
    els: Optional[list]
    pinned: bool = False


@dataclass
class For:
    var: str
    hi: object
    body: list
    pinned: bool = False


@dataclass
class ForEach:
    var: str
    coll: object
    body: list
    pinned: bool = False


@dataclass
class While:
    """{ int var = 0; while (var < hi) { var += 1; body } }"""
    var: str
    hi: object
    body: list
    pinned: bool = False


@dataclass
class Break:
    pinned: bool = False


@dataclass
class Continue:
    pinned: bool = False


@dataclass
class Return:
    e: object
    pinned: bool = False


@dataclass
class Print:
    args: list
    pinned: bool = False


@dataclass
class Reject:
    args: list
    pinned: bool = False


@dataclass
class Target:
    e: object
    pinned: bool = False


@dataclass
class Tilde:
    lhs: object
    dist: str
    args: list
    fn: object = None
    pinned: bool = False


@dataclass
class CallStmt:
    call: object
    pinned: bool = False


@dataclass
class Block:
    body: list
    pinned: bool = False


@dataclass(eq=False)
class FunDef:
    """kind is plain, void, lpdf, lpmf, rng or lp; name has the suffix."""
    name: str
    kind: str
    params: list
    ret: Optional[T]
    body: list
    recursive: bool = False

    def effects(self):
        return function_effects(self)


@dataclass
class DataVar:
    """role is size (the value of sizes[sym]), values, or index (values in
    1..sizes[target])."""
    name: str
    t: T
    role: str
    sym: int = 0
    target: int = 0
    scale: float = 1.0


@dataclass
class Param:
    name: str
    t: T


BLOCKS = ("td", "tp", "model", "gq")
BLOCK_TITLES = {"td": "transformed data", "tp": "transformed parameters",
                "model": "model", "gq": "generated quantities"}


class Program:
    """A generated program. datavars are the data block's variables; data()
    is the matching data."""

    def __init__(self, seed, sizes, config, datavars=None, functions=None,
                 td=None, params=None, tp=None, model=None, gq=None):
        self.seed, self.sizes, self.config = seed, sizes, config
        self.datavars = datavars or []
        self.functions = functions or []
        self.td, self.params = td or [], params or []
        self.tp, self.model, self.gq = tp or [], model or [], gq or []

    def source(self):
        return program_source(self)

    def data(self):
        return program_data(self)

    def features(self):
        return program_features(self)


# ---------------------------------------------------------------------------
# Effects.

RNG_BUILTINS = ("normal_rng", "poisson_log_rng", "bernoulli_logit_rng")


def function_effects(f, seen=None):
    """{"print", "reject", "rng", "target"} that calling f can cause."""
    seen = set() if seen is None else seen
    if id(f) in seen:
        return frozenset()
    seen.add(id(f))
    out = set()
    if f.kind == "lp":
        out.add("target")
    for s in walk_stmts(f.body):
        if isinstance(s, Print):
            out.add("print")
        elif isinstance(s, Reject):
            out.add("reject")
        elif isinstance(s, (Target, Tilde)):
            out.add("target")
        for e in stmt_exprs(s):
            for x in walk_expr(e):
                if isinstance(x, Call):
                    if x.fn is not None:
                        out |= function_effects(x.fn, seen)
                    elif x.name in RNG_BUILTINS:
                        out.add("rng")
    return frozenset(out)


def call_effects(c):
    if not isinstance(c, Call):
        return frozenset()
    if c.fn is not None:
        return function_effects(c.fn)
    if c.name in RNG_BUILTINS:
        return frozenset({"rng"})
    return frozenset()


def effect_calls(e):
    return sum(1 for x in walk_expr(e) if call_effects(x))


# ---------------------------------------------------------------------------
# Traversal.


def expr_children(e):
    if isinstance(e, Size):
        return [e.via] if e.via is not None else []
    if isinstance(e, (Op, Div)):
        return [e.a, e.b]
    if isinstance(e, Call):
        return list(e.args)
    if isinstance(e, Cond):
        return [e.c, e.a, e.b]
    if isinstance(e, Index):
        return [e.base] + [i for _, i in e.idx]
    if isinstance(e, Proj):
        return [e.base]
    if isinstance(e, (Tup, ArrE)):
        return list(e.items)
    if isinstance(e, Clamp):
        return [e.e, e.hi]
    return []


def walk_expr(e):
    yield e
    for c in expr_children(e):
        yield from walk_expr(c)


def stmt_exprs(s):
    """A statement's own expressions, not those of nested statements."""
    if isinstance(s, Decl):
        return list(s.sizes) + ([s.init] if s.init is not None else [])
    if isinstance(s, Assign):
        return [s.lhs, s.rhs]
    if isinstance(s, If):
        return [s.cond]
    if isinstance(s, (For, While)):
        return [s.hi]
    if isinstance(s, ForEach):
        return [s.coll]
    if isinstance(s, Return):
        return [s.e] if s.e is not None else []
    if isinstance(s, (Print, Reject)):
        return [a for a in s.args if not isinstance(a, str)]
    if isinstance(s, Target):
        return [s.e]
    if isinstance(s, Tilde):
        return [s.lhs] + list(s.args)
    if isinstance(s, CallStmt):
        return [s.call]
    return []


def stmt_bodies(s):
    if isinstance(s, If):
        return [s.then] + ([s.els] if s.els is not None else [])
    if isinstance(s, (For, ForEach, While, Block)):
        return [s.body]
    return []


def walk_stmts(body):
    for s in body:
        yield s
        for b in stmt_bodies(s):
            yield from walk_stmts(b)


def all_bodies(p):
    """(where, body) for every function and block; where is a FunDef or a
    block name."""
    out = [(f, f.body) for f in p.functions]
    return out + [(b, getattr(p, b)) for b in BLOCKS]


def all_exprs(p):
    for _, body in all_bodies(p):
        for s in walk_stmts(body):
            for e in stmt_exprs(s):
                yield from walk_expr(e)


# ---------------------------------------------------------------------------
# Printing.


def real_str(v):
    s = f"{v:.2f}"
    return "0.00" if s == "-0.00" else s


def elem_value(v, i):
    x = v + 0.25 * i
    return round(((x + 1.5) % 3.0) - 1.5, 2) if abs(x) > 1.5 else round(x, 2)


def lit_str(t, v, p, i=0):
    k = t.kind
    if k == "int":
        return str(int(v))
    if k == "real":
        return real_str(elem_value(v, i))
    if k in ("vector", "row_vector"):
        n = p.sizes[t.dims[0]]
        if n == 0:
            return f"rep_{k}(0.0, 0)"
        body = "[" + ", ".join(real_str(elem_value(v, i + j))
                               for j in range(n)) + "]"
        return body + "'" if k == "vector" else body
    if k == "matrix":
        return (f"rep_matrix({real_str(elem_value(v, i))}, "
                f"{p.sizes[t.dims[0]]}, {p.sizes[t.dims[1]]})")
    if k == "array":
        n = p.sizes[t.dims[0]]
        if n == 0:
            return f"rep_array({lit_str(t.elem, v, p, i)}, 0)"
        step = 1 if t.elem.kind == "int" else 2
        return "{" + ", ".join(lit_str(t.elem, v + (j if t.elem.kind == "int"
                                                     else 0), p,
                                       i + step * j)
                               for j in range(n)) + "}"
    if k == "tuple":
        return "(" + ", ".join(lit_str(it, v, p, i + j)
                               for j, it in enumerate(t.items)) + ")"
    raise ValueError(k)


SIZE_FUNCS = {"rows": "rows", "cols": "cols", "size": "size",
              "num": "num_elements"}


def expr_str(e, p):
    def s(x):
        return expr_str(x, p)

    if isinstance(e, Var):
        return e.name
    if isinstance(e, Lit):
        return lit_str(e.t, e.v, p)
    if isinstance(e, Size):
        if e.how == "lit":
            return str(p.sizes[e.sym])
        if e.how == "data":
            return f"N{e.sym}"
        return f"{SIZE_FUNCS[e.how]}({s(e.via)})"
    if isinstance(e, Op):
        return f"({s(e.a)} {e.op} {s(e.b)})"
    if isinstance(e, Call):
        if e.name == "'":
            return f"({s(e.args[0])})'"
        if e.bar:
            rest = ", ".join(s(a) for a in e.args[1:])
            return f"{e.name}({s(e.args[0])} | {rest})"
        return f"{e.name}({', '.join(s(a) for a in e.args)})"
    if isinstance(e, Div):
        return f"({s(e.a)} / (1 + square({s(e.b)})))"
    if isinstance(e, Cond):
        return f"({s(e.c)} ? {s(e.a)} : {s(e.b)})"
    if isinstance(e, Index):
        parts = [s(i) if k != "upto" else f"1:{s(i)}" for k, i in e.idx]
        return f"{s(e.base)}[{', '.join(parts)}]"
    if isinstance(e, Proj):
        return f"{s(e.base)}.{e.k}"
    if isinstance(e, Tup):
        return "(" + ", ".join(s(i) for i in e.items) + ")"
    if isinstance(e, Clamp):
        return f"max(1, min({s(e.e)}, {s(e.hi)}))"
    if isinstance(e, ArrE):
        return "{" + ", ".join(s(i) for i in e.items) + "}"
    raise TypeError(type(e))


def stmt_lines(st, p, ind, out):
    pad = "  " * ind

    def s(x):
        return expr_str(x, p)

    def body(b, extra=()):
        for line in extra:
            out.append("  " * (ind + 1) + line)
        for x in b:
            stmt_lines(x, p, ind + 1, out)

    if isinstance(st, Decl):
        ty = sized(st.t, iter([s(x) for x in st.sizes]))
        init = f" = {s(st.init)}" if st.init is not None else ""
        out.append(f"{pad}{ty} {st.name}{init};")
    elif isinstance(st, Assign):
        out.append(f"{pad}{s(st.lhs)} {st.op} {s(st.rhs)};")
    elif isinstance(st, If):
        out.append(f"{pad}if ({s(st.cond)}) {{")
        body(st.then)
        els = st.els
        while els is not None and len(els) == 1 and isinstance(els[0], If):
            out.append(f"{pad}}} else if ({s(els[0].cond)}) {{")
            body(els[0].then)
            els = els[0].els
        if els is not None:
            out.append(f"{pad}}} else {{")
            body(els)
        out.append(f"{pad}}}")
    elif isinstance(st, For):
        out.append(f"{pad}for ({st.var} in 1:{s(st.hi)}) {{")
        body(st.body)
        out.append(f"{pad}}}")
    elif isinstance(st, ForEach):
        out.append(f"{pad}for ({st.var} in {s(st.coll)}) {{")
        body(st.body)
        out.append(f"{pad}}}")
    elif isinstance(st, While):
        # The braces keep the counter local, so it is never a block-level
        # variable (an int transformed parameter or a written output).
        out.append(f"{pad}{{")
        out.append(f"{pad}  int {st.var} = 0;")
        out.append(f"{pad}  while ({st.var} < {s(st.hi)}) {{")
        inner_lines = [f"{pad}    {st.var} += 1;"]
        for x in st.body:
            stmt_lines(x, p, ind + 2, inner_lines)
        out.extend(inner_lines)
        out.append(f"{pad}  }}")
        out.append(f"{pad}}}")
    elif isinstance(st, Break):
        out.append(f"{pad}break;")
    elif isinstance(st, Continue):
        out.append(f"{pad}continue;")
    elif isinstance(st, Return):
        out.append(f"{pad}return{'' if st.e is None else ' ' + s(st.e)};")
    elif isinstance(st, (Print, Reject)):
        word = "print" if isinstance(st, Print) else "reject"
        args = ", ".join(a if isinstance(a, str) else s(a) for a in st.args)
        out.append(f"{pad}{word}({args});")
    elif isinstance(st, Target):
        out.append(f"{pad}target += {s(st.e)};")
    elif isinstance(st, Tilde):
        out.append(f"{pad}{s(st.lhs)} ~ {st.dist}("
                   f"{', '.join(s(a) for a in st.args)});")
    elif isinstance(st, CallStmt):
        out.append(f"{pad}{s(st.call)};")
    elif isinstance(st, Block):
        out.append(f"{pad}{{")
        body(st.body)
        out.append(f"{pad}}}")
    else:
        raise TypeError(type(st))


def program_source(p):
    out = ["functions {"]
    for f in p.functions:
        ret = "void" if f.ret is None else unsized(f.ret)
        params = ", ".join(f"{unsized(t)} {n}" for n, t in f.params)
        out.append(f"  {ret} {f.name}({params}) {{")
        for st in f.body:
            stmt_lines(st, p, 2, out)
        out.append("  }")
    out.append("}")
    out.append("data {")
    for d in p.datavars:
        if d.role == "size":
            out.append(f"  int<lower=0> {d.name};")
        else:
            ty = sized(d.t, iter([f"N{s}" for s in decl_syms(d.t)]))
            out.append(f"  {ty} {d.name};")
    out.append("}")
    for b in ("td",):
        out.append(f"{BLOCK_TITLES[b]} {{")
        for st in getattr(p, b):
            stmt_lines(st, p, 1, out)
        out.append("}")
    out.append("parameters {")
    for q in p.params:
        ty = sized(q.t, iter([f"N{s}" for s in decl_syms(q.t)]))
        out.append(f"  {ty} {q.name};")
    out.append("}")
    for b in ("tp", "model", "gq"):
        out.append(f"{BLOCK_TITLES[b]} {{")
        for st in getattr(p, b):
            stmt_lines(st, p, 1, out)
        out.append("}")
    return "\n".join(out) + "\n"


def data_value(t, p, rng, role, target, scale):
    k = t.kind
    if k == "int":
        if role == "index":
            n = p.sizes[target]
            return 1 + rng.randrange(n) if n else 1
        return rng.randrange(4) if scale else 0
    if k == "real":
        return round(rng.uniform(-1.5, 1.5) * scale, 3)
    if k in ("vector", "row_vector", "array"):
        n = p.sizes[t.dims[0]]
        elem = t.elem if k == "array" else REAL
        return [data_value(elem, p, rng, role, target, scale)
                for _ in range(n)]
    if k == "matrix":
        return [[data_value(REAL, p, rng, role, target, scale)
                 for _ in range(p.sizes[t.dims[1]])]
                for _ in range(p.sizes[t.dims[0]])]
    raise ValueError(k)


def program_data(p):
    out = {}
    for d in p.datavars:
        if d.role == "size":
            out[d.name] = p.sizes[d.sym]
            continue
        rng = random.Random(f"{p.seed}:{d.name}")
        out[d.name] = data_value(d.t, p, rng, d.role, d.target, d.scale)
    return out


# ---------------------------------------------------------------------------
# Rules.

ALLOWED_CALLS = {
    "tanh", "inv_logit", "square", "log1p_exp", "exp", "sum", "dot_self",
    "rows", "cols", "size", "num_elements", "rep_vector", "rep_row_vector",
    "rep_matrix", "rep_array", "to_vector", "dot_product", "min", "max",
    "'", "normal_lpdf", "normal_lupdf", "cauchy_lpdf", "cauchy_lupdf",
    "logistic_lpdf", "logistic_lupdf", "poisson_log_lpmf",
    "poisson_log_lupmf", "bernoulli_logit_lpmf", "bernoulli_logit_lupmf",
} | set(RNG_BUILTINS)
ALLOWED_OPS = {"+", "-", "*", "<", ">", "<=", ">=", "==", "&&", "||"}
DIST_NAMES = {"normal", "cauchy", "logistic", "poisson_log",
              "bernoulli_logit"}


def ival(e, p):
    """The value of an int expression when it is known statically."""
    if isinstance(e, Lit) and e.t == INT:
        return int(e.v)
    if isinstance(e, Size):
        return p.sizes[e.sym]
    if isinstance(e, Call) and e.name in ("min", "max") and e.t == INT:
        vals = [ival(a, p) for a in e.args]
        if None not in vals:
            return min(vals) if e.name == "min" else max(vals)
    if isinstance(e, Op) and e.op in ("+", "-") and e.t == INT:
        a, b = ival(e.a, p), ival(e.b, p)
        if a is not None and b is not None:
            return a + b if e.op == "+" else a - b
    return None


def upper(e, p):
    """An upper bound of an int expression, or None."""
    v = ival(e, p)
    if v is not None:
        return v
    if isinstance(e, Call) and e.name in ("min", "max") and e.t == INT:
        ups = [upper(a, p) for a in e.args]
        if e.name == "min":
            known = [u for u in ups if u is not None]
            return min(known) if known else None
        return None if None in ups else max(ups)
    if isinstance(e, Clamp):
        return upper(e.hi, p)
    return None


def dim_value(t, pos, p):
    """The extent of the pos-th index of type t."""
    if t.kind == "matrix":
        return p.sizes[t.dims[pos]]
    return p.sizes[t.dims[0]]


class _Checker:
    def __init__(self, p):
        self.p = p
        self.v = []

    def bad(self, rule, msg):
        self.v.append(f"{rule}: {msg}")

    def check(self):
        p = self.p
        names = {d.name for d in p.datavars} | {q.name for q in p.params}
        for f in p.functions:
            scope = {n: None for n, _ in f.params}
            self.body(f.body, scope, ctx_for(f, None), 0)
            if f.recursive:
                self.recursion(f)
        scope = {n: None for n in names}
        for b in BLOCKS:
            # Parameters, transformed parameters and model locals are not
            # visible in transformed data.
            here = dict(scope) if b != "td" else {
                d.name: None for d in p.datavars}
            if b == "td":
                self.body(p.td, here, ctx_for(None, b), 0)
                scope.update(declared(p.td))
            else:
                self.body(getattr(p, b), here, ctx_for(None, b), 0)
                if b in ("tp",):
                    scope.update(declared(p.tp))
        return self.v

    def recursion(self, f):
        n = f.params[0][0]
        first = f.body[0] if f.body else None
        ok = (isinstance(first, If) and isinstance(first.cond, Op)
              and first.cond.op == "<=" and first.cond.a == Var(n, INT)
              and ival(first.cond.b, self.p) == 0
              and any(isinstance(s, Return) for s in first.then))
        if not ok:
            self.bad("bounded", f"{f.name} has no base case first")
        for e in all_exprs_of(f.body):
            if isinstance(e, Call) and e.fn is f:
                a = e.args[0]
                down = (isinstance(a, Op) and a.op == "-" and
                        a.a == Var(n, INT) and ival(a.b, self.p) == 1)
                if not down and (upper(a, self.p) is None or
                                 upper(a, self.p) > 0):
                    self.bad("bounded", f"{f.name} recurses without "
                             "decreasing")

    def body(self, body, scope, ctx, loops):
        scope = dict(scope)
        for i, s in enumerate(body):
            if isinstance(s, Decl) and not self.p.config.get("out_of_range"):
                for z, sym in zip(s.sizes, decl_syms(s.t)):
                    if ival(z, self.p) != self.p.sizes[sym]:
                        self.bad("in_range", f"{s.name} is declared with "
                                 f"size {expr_str(z, self.p)} for extent "
                                 f"{self.p.sizes[sym]}")
            if isinstance(s, Decl) and s.init is None:
                nxt = body[i + 1] if i + 1 < len(body) else None
                if not (isinstance(nxt, Assign) and nxt.op == "=" and
                        nxt.lhs == Var(s.name, s.t) and
                        not reads(nxt.rhs, s.name)):
                    self.bad("assigned", f"{s.name} is declared without a "
                             "value and not assigned next")
            self.stmt(s, scope, ctx, loops)
            if isinstance(s, Decl):
                scope[s.name] = None

    def stmt(self, s, scope, ctx, loops):
        p = self.p
        for e in stmt_exprs(s):
            self.expr(e, scope, ctx)
        n = sum(effect_calls(e) for e in stmt_exprs(s))
        if isinstance(s, CallStmt) and call_effects(s.call):
            n = max(n, 1)
        if n > 1:
            self.bad("one_effect", f"{n} calls with effects in one "
                     "statement")
        if isinstance(s, Tilde) and s.fn is None and s.dist in DENSITIES \
                and not positive_scale(s.args[1], p):
            self.bad("finite", f"{s.dist} scale {expr_str(s.args[1], p)}")
        if isinstance(s, (Target, Tilde)) and not ctx["target"]:
            self.bad("block", "target increment outside the model block "
                     "and _lp functions")
        if isinstance(s, (For, While)):
            hi = upper(s.hi, p)
            if hi is None or hi > MAX_TRIPS:
                self.bad("bounded", f"loop over 1:{expr_str(s.hi, p)}")
            inner = dict(scope)
            inner[s.var] = hi
            self.body(s.body, inner, ctx, loops + 1)
        elif isinstance(s, ForEach):
            n_iter = container_len(s.coll.t, p)
            if n_iter is None or n_iter > MAX_TRIPS:
                self.bad("bounded", "foreach over a long container")
            inner = dict(scope)
            inner[s.var] = None
            self.body(s.body, inner, ctx, loops + 1)
        else:
            for b in stmt_bodies(s):
                self.body(b, scope, ctx, loops)

    def expr(self, e, scope, ctx):
        p = self.p
        for x in walk_expr(e):
            if isinstance(x, Var) and x.name not in scope:
                self.bad("assigned", f"{x.name} is read but not declared")
            if isinstance(x, Op) and x.op not in ALLOWED_OPS:
                self.bad("finite", f"operator {x.op}")
            if isinstance(x, Call) and x.fn is None:
                if x.name not in ALLOWED_CALLS:
                    self.bad("finite", f"call to {x.name}")
                elif x.name == "exp" and not (
                        isinstance(x.args[0], Call) and
                        x.args[0].name == "tanh"):
                    self.bad("finite", "exp of something other than tanh")
                for i in SCALE_ARGS.get(x.name, ()):
                    if not positive_scale(x.args[i], p):
                        self.bad("finite", f"{x.name} scale "
                                 f"{expr_str(x.args[i], p)}")
                if x.name in RATE_ARGS and not bounded_rate(
                        x.args[RATE_ARGS[x.name]], p):
                    self.bad("finite", f"{x.name} log rate "
                             f"{expr_str(x.args[RATE_ARGS[x.name]], p)}")
            effects = call_effects(x)
            if "rng" in effects and not ctx["rng"]:
                self.bad("block", f"{x.name} where rng is not allowed")
            if "target" in effects and not ctx["lp"]:
                self.bad("block", f"{x.name} where _lp is not allowed")
            if isinstance(x, Call) and x.name.endswith(("_lupdf", "_lupmf")) \
                    and not ctx["lupdf"]:
                self.bad("block", f"{x.name} where _lupdf is not allowed")
            if isinstance(x, Call) and x.fn is not None and x.fn.recursive \
                    and x.fn is not ctx.get("fn"):
                hi = upper(x.args[0], p)
                if hi is None or hi > MAX_RECURSION:
                    self.bad("bounded", f"{x.name} may recurse more than "
                             f"{MAX_RECURSION} deep")
            if self.p.config.get("out_of_range", 0):
                continue
            if isinstance(x, Index):
                self.index(x, scope)
            if isinstance(x, ArrE) and len(x.items) != p.sizes[x.t.dims[0]]:
                self.bad("in_range", "array expression length differs from "
                         "its size")

    def index(self, x, scope):
        p = self.p
        t = x.base.t
        pos = 0
        for kind, i in x.idx:
            n = dim_value(t, pos, p)
            if kind == "one":
                if isinstance(i, Clamp):
                    hi = upper(i.hi, p)
                    if n < 1 or hi is None or hi > n:
                        self.bad("in_range", f"clamped index into extent "
                                 f"{n}")
                elif isinstance(i, Var) and scope.get(i.name) is not None:
                    if scope[i.name] > n:
                        self.bad("in_range", f"loop variable {i.name} up "
                                 f"to {scope[i.name]} indexes extent {n}")
                else:
                    v = ival(i, p)
                    if not isinstance(v, int) or not 1 <= v <= n:
                        self.bad("in_range", f"index {expr_str(i, p)} "
                                 f"into extent {n}")
            elif kind == "upto":
                v = ival(i, p)
                if not isinstance(v, int) or not 0 <= v <= n:
                    self.bad("in_range", f"slice 1:{expr_str(i, p)} of "
                             f"extent {n}")
            elif kind == "multi":
                if not (isinstance(i, Var) and self.index_data_ok(i, n)):
                    self.bad("in_range", "multi-index that is not a data "
                             "index array")
            if t.kind == "matrix" and pos == 0 and len(x.idx) == 2:
                pos = 1
                continue
            t = t.elem if t.kind == "array" else t
            pos = 0

    def index_data_ok(self, i, n):
        for d in self.p.datavars:
            if d.name == i.name and d.role == "index":
                return self.p.sizes[d.t.dims[0]] == 0 or (
                    self.p.sizes[d.target] == n and n >= 1)
        return False


# Arguments that must stay positive (a scale) or bounded (a log rate).
SCALE_ARGS = {f"{d}_{k}": (2,) for d in ("normal", "cauchy", "logistic")
              for k in ("lpdf", "lupdf")}
SCALE_ARGS["normal_rng"] = (1,)
RATE_ARGS = {"poisson_log_lpmf": 1, "poisson_log_lupmf": 1,
             "poisson_log_rng": 0}


def positive_scale(e, p):
    if isinstance(e, Lit):
        return e.t == REAL and elem_value(e.v, 0) > 0
    return (isinstance(e, Call) and e.name == "exp" and
            isinstance(e.args[0], Call) and e.args[0].name == "tanh")


def bounded_rate(e, p):
    if isinstance(e, Lit):
        return e.t == REAL and abs(elem_value(e.v, 0)) <= 1.5
    return isinstance(e, Call) and e.name == "tanh"


def container_len(t, p):
    if t.kind in ("vector", "row_vector", "array"):
        return p.sizes[t.dims[0]]
    if t.kind == "matrix":
        return p.sizes[t.dims[0]] * p.sizes[t.dims[1]]
    return None


def declared(body):
    return {s.name: None for s in body if isinstance(s, Decl)}


def reads(e, name):
    return any(isinstance(x, Var) and x.name == name for x in walk_expr(e))


def all_exprs_of(body):
    for s in walk_stmts(body):
        for e in stmt_exprs(s):
            yield from walk_expr(e)


def ctx_for(f, block):
    """What a function body or block may do."""
    if f is not None:
        k = f.kind
        return {"fn": f, "rng": k == "rng", "lp": k == "lp",
                "target": k == "lp", "lupdf": k in ("lpdf", "lpmf")}
    return {"fn": None, "rng": block in ("td", "gq"),
            "lp": block in ("tp", "model"), "target": block == "model",
            "lupdf": block == "model"}


def check_rules(p):
    """The rule violations in p, each "rule: message"; empty when it keeps
    all six."""
    return _Checker(p).check()


# ---------------------------------------------------------------------------
# Features. Computed from the AST, so a reduced program reports its own.

FEATURES = (
    "t_int", "t_real", "t_vector", "t_row_vector", "t_matrix", "t_array_int",
    "t_array_real", "t_array_vector", "t_tuple", "t_tuple_nested",
    "t_tuple_arg", "t_tuple_return", "z_empty",
    "fn_plain", "fn_void", "fn_lpdf", "fn_lpmf", "fn_rng", "fn_lp",
    "fn_overload", "fn_recursive",
    "s_decl", "s_assign", "s_compound_assign", "s_indexed_assign",
    "s_tuple_assign", "s_if", "s_else_if", "s_for", "s_foreach", "s_while",
    "s_nested_loop", "s_break", "s_continue", "s_early_return",
    "s_return_in_for", "s_return_in_while", "s_return_in_nested_loop",
    "s_void_return", "s_print", "s_reject", "s_block",
    "s_target", "s_tilde_user", "s_lupdf_user", "s_void_call",
    "c_literal_arg", "c_var_arg", "c_data_and_param_args", "c_expr_arg",
    "c_slice_arg", "c_multi_index_arg", "c_nested_call", "c_call_in_size",
    "c_call_loop_bound", "c_call_in_cond", "c_unused_result",
    "c_call_in_td", "c_rng_in_arg", "c_print_in_arg", "c_reject_in_arg",
    "c_call_in_earlier_fn", "c_effect_in_tuple_arg", "c_param_read_twice",
)


def type_features(t, out):
    if t.kind == "array":
        out.add({"int": "t_array_int", "real": "t_array_real",
                 "vector": "t_array_vector"}.get(t.elem.kind, "t_array"))
        type_features(t.elem, out)
    elif t.kind == "tuple":
        out.add("t_tuple")
        for i in t.items:
            if i.kind == "tuple":
                out.add("t_tuple_nested")
            type_features(i, out)
    else:
        out.add("t_" + t.kind)


def program_features(p):
    out = set()
    if 0 in p.sizes:
        out.add("z_empty")
    names = {}
    for f in p.functions:
        names.setdefault(f.name, []).append(f)
        out.add("fn_" + f.kind)
        if f.recursive:
            out.add("fn_recursive")
        for _, t in f.params:
            type_features(t, out)
            if has_tuple(t):
                out.add("t_tuple_arg")
        if f.ret is not None:
            type_features(f.ret, out)
            if has_tuple(f.ret):
                out.add("t_tuple_return")
    if any(len(v) > 1 for v in names.values()):
        out.add("fn_overload")
    for f in p.functions:
        for n, _ in f.params:
            top = sum(1 for st in f.body for e in stmt_exprs(st)
                      for x in walk_expr(e) if x == Var(n, x.t)
                      if isinstance(x, Var))
            if top >= 2:
                out.add("c_param_read_twice")
    for d in p.datavars:
        type_features(d.t, out)
    for q in p.params:
        type_features(q.t, out)
    callers = {}
    readers = {e.name for e in all_exprs(p) if isinstance(e, Var)}
    for where, body in all_bodies(p):
        _fb(body, out, where, [], callers, p, readers)
    for f, kinds in callers.items():
        if {"data", "param"} <= kinds:
            out.add("c_data_and_param_args")
    return sorted(out & set(FEATURES))


def _fb(body, out, where, loops, callers, p, readers):
    in_fn = isinstance(where, FunDef)
    for s in body:
        k = type(s).__name__
        if isinstance(s, Decl):
            out.add("s_decl")
            type_features(s.t, out)
            if s.init is not None and isinstance(s.init, Call) and \
                    s.init.fn is not None and s.name not in readers:
                out.add("c_unused_result")
            if s.init is not None and any(isinstance(x, Call) and
                                          x.fn is not None
                                          for x in walk_expr(s.init)) \
                    and s.name not in readers:
                out.add("c_unused_result")
            for z in s.sizes:
                if any(isinstance(x, Call) and x.fn is not None
                       for x in walk_expr(z)):
                    out.add("c_call_in_size")
        elif isinstance(s, Assign):
            if s.op != "=":
                out.add("s_compound_assign")
            elif isinstance(s.lhs, Index):
                out.add("s_indexed_assign")
            elif isinstance(s.lhs, Proj):
                out.add("s_tuple_assign")
            else:
                out.add("s_assign")
            if isinstance(s.lhs, Proj):
                out.add("s_tuple_assign")
            if isinstance(s.lhs, Index):
                out.add("s_indexed_assign")
        elif isinstance(s, If):
            out.add("s_if")
            if s.els is not None and len(s.els) == 1 and \
                    isinstance(s.els[0], If):
                out.add("s_else_if")
            if any(isinstance(x, Call) and x.fn is not None
                   for x in walk_expr(s.cond)):
                out.add("c_call_in_cond")
        elif isinstance(s, (For, While, ForEach)):
            out.add({"For": "s_for", "While": "s_while",
                     "ForEach": "s_foreach"}[k])
            if loops:
                out.add("s_nested_loop")
            if any(isinstance(x, Call) and x.fn is not None
                   for e in stmt_exprs(s) for x in walk_expr(e)):
                out.add("c_call_loop_bound")
        elif isinstance(s, Break):
            out.add("s_break")
        elif isinstance(s, Continue):
            out.add("s_continue")
        elif isinstance(s, Return):
            if in_fn and s is not where.body[-1]:
                if s.e is None:
                    out.add("s_void_return")
                else:
                    out.add("s_early_return")
                if "For" in loops or "ForEach" in loops:
                    out.add("s_return_in_for")
                if "While" in loops:
                    out.add("s_return_in_while")
                if len(loops) > 1:
                    out.add("s_return_in_nested_loop")
        elif isinstance(s, Print):
            out.add("s_print")
        elif isinstance(s, Reject):
            out.add("s_reject")
        elif isinstance(s, Block):
            out.add("s_block")
        elif isinstance(s, Target):
            out.add("s_target")
            for x in walk_expr(s.e):
                if isinstance(x, Call) and x.fn is not None and \
                        x.name.endswith(("_lupdf", "_lupmf")):
                    out.add("s_lupdf_user")
        elif isinstance(s, Tilde):
            out.add("s_target")
            if s.fn is not None:
                out.add("s_tilde_user")
        elif isinstance(s, CallStmt):
            out.add("s_void_call")
        for e in stmt_exprs(s):
            _features_expr(e, out, where, callers, p, nested=False)
        for b in stmt_bodies(s):
            inner = loops + [k] if isinstance(s, (For, While, ForEach)) \
                else loops
            _fb(b, out, where, inner, callers, p, readers)


def _features_expr(e, out, where, callers, p, nested):
    if isinstance(e, Call) and e.fn is not None:
        if nested:
            out.add("c_nested_call")
        if where == "td":
            out.add("c_call_in_td")
        if isinstance(where, FunDef) and e.fn is not where:
            out.add("c_call_in_earlier_fn")
        kinds = callers.setdefault(id(e.fn), set())
        if where in ("td",):
            kinds.add("data")
        if where in ("model", "tp"):
            if any(isinstance(x, Var) and x.name in
                   {q.name for q in p.params} for x in walk_expr(e)):
                kinds.add("param")
        if where == "gq":
            kinds.add("data")
        for (_, pt), a in zip(e.fn.params, e.args):
            if has_tuple(pt) and not isinstance(a, (Var, Lit)) and \
                    any(call_effects(x) for x in walk_expr(a)):
                out.add("c_effect_in_tuple_arg")
        for a in e.args:
            if isinstance(a, Lit):
                out.add("c_literal_arg")
            elif isinstance(a, Var):
                out.add("c_var_arg")
            elif isinstance(a, Index) and any(k == "upto" for k, _ in a.idx):
                out.add("c_slice_arg")
            elif isinstance(a, Index) and any(k == "multi" for k, _ in a.idx):
                out.add("c_multi_index_arg")
            else:
                out.add("c_expr_arg")
            for x in walk_expr(a):
                eff = call_effects(x)
                if "rng" in eff:
                    out.add("c_rng_in_arg")
                if "print" in eff:
                    out.add("c_print_in_arg")
                if "reject" in eff:
                    out.add("c_reject_in_arg")
            _features_expr(a, out, where, callers, p, nested=True)
        return
    if isinstance(e, Size) and isinstance(e.via, Call) and \
            e.via.fn is not None:
        out.add("c_call_in_size")
    for c in expr_children(e):
        _features_expr(c, out, where, callers, p, nested)


# ---------------------------------------------------------------------------
# Configuration.

DEFAULT_WEIGHTS = {
    # Types of declarations, parameters and arguments.
    "int": 1.0, "real": 3.0, "vector": 2.0, "row_vector": 1.0,
    "matrix": 0.7, "array_int": 0.7, "array_real": 1.0,
    "array_vector": 0.7, "tuple": 1.0, "tuple_nested": 0.3,
    # Function kinds.
    "fn_plain": 3.0, "fn_void": 1.0, "fn_lpdf": 1.0, "fn_lpmf": 0.6,
    "fn_rng": 1.2, "fn_lp": 0.8, "fn_overload": 0.3, "fn_recursive": 0.4,
    # Statements.
    "decl": 3.0, "assign": 1.5, "compound_assign": 1.0,
    "indexed_assign": 1.0, "tuple_assign": 0.6, "if": 1.5, "else_if": 0.4,
    "for": 1.0, "foreach": 0.6, "while": 0.6, "break": 0.5,
    "continue": 0.5, "early_return": 0.8, "print": 0.5, "reject": 0.25,
    "block": 0.6, "target": 1.0, "tilde": 1.0, "lupdf": 0.5,
    "void_call": 0.8, "unused_draw": 0.6,
    # Expressions and call sites.
    "var": 3.0, "literal": 1.0, "call": 2.0, "effect_call": 1.0,
    "slice": 0.6, "multi_index": 0.5, "call_in_size": 0.5,
    "call_loop_bound": 0.4, "call_in_cond": 0.5, "ternary": 0.3,
    "logic": 0.3, "tuple_arg": 1.0, "effect_in_tuple": 1.0,
    "param_reuse": 1.0,
}

DEFAULT_CONFIG = {
    "functions": [1, 4],
    "body_stmts": [1, 4],
    "block_stmts": [1, 4],
    "expr_depth": 3,
    "stmt_depth": 2,
    "sizes": [1, 3],
    "out_of_range": 0.0,
    "weights": DEFAULT_WEIGHTS,
}


def config(overrides=None):
    """DEFAULT_CONFIG with `overrides` merged in, weights merged by key."""
    cfg = json.loads(json.dumps(DEFAULT_CONFIG))
    for k, v in (overrides or {}).items():
        if k == "weights":
            cfg["weights"].update(v)
        else:
            cfg[k] = v
    return cfg


def load_config(path):
    if path is None:
        return config()
    with open(path) as f:
        return config(json.load(f))


# ---------------------------------------------------------------------------
# Generation.


@dataclass
class VarInfo:
    name: str
    t: T
    mutable: bool = False
    bound: Optional[int] = None  # an int loop variable's largest value
    param: bool = False
    data: bool = False


class Ctx:
    """Where an expression or statement is generated."""

    def __init__(self, fn, block, scope, loops=0, budget=1, in_loop=False):
        self.fn, self.block = fn, block
        self.scope = list(scope)
        self.loops = loops
        self.in_loop = in_loop
        self.budget = [budget]
        a = ctx_for(fn, block)
        self.rng, self.lp, self.target, self.lupdf = (
            a["rng"], a["lp"], a["target"], a["lupdf"])

    def child(self, extra=(), loop=False):
        c = Ctx.__new__(Ctx)
        c.__dict__.update(self.__dict__)
        c.scope = self.scope + list(extra)
        c.loops = self.loops + (1 if loop else 0)
        c.in_loop = self.in_loop or loop
        c.budget = [1]
        return c

    def fresh(self):
        """The same place, for a new statement with its own effect budget."""
        c = Ctx.__new__(Ctx)
        c.__dict__.update(self.__dict__)
        c.budget = [1]
        return c

    def data_only(self):
        """For the sizes of a block-level declaration, which Stan requires
        to be data. Shares this statement's effect budget."""
        c = Ctx.__new__(Ctx)
        c.__dict__.update(self.__dict__)
        c.scope = [v for v in self.scope if v.data]
        c.rng = c.lp = False
        return c

    def without(self, name):
        c = Ctx.__new__(Ctx)
        c.__dict__.update(self.__dict__)
        c.scope = [v for v in self.scope if v.name != name]
        return c


REAL_UNARY = ("tanh", "inv_logit", "log1p_exp", "square")
DENSITIES = ("normal", "cauchy", "logistic")


class Gen:
    def __init__(self, seed, cfg):
        self.r = random.Random(seed)
        self.cfg = cfg
        self.wt = cfg["weights"]
        self.p = Program(seed=seed, sizes=[], config=cfg)
        self.n = 0
        self.depth = cfg["expr_depth"]

    # -- helpers
    def name(self, prefix):
        self.n += 1
        return f"{prefix}{self.n}"

    def w(self, key):
        return self.wt.get(key, 0.0)

    def chance(self, key, scale=0.3):
        return self.r.random() < min(1.0, self.w(key) * scale)

    def pick(self, options):
        """options: [(weight, value)]; a value, or None if all are zero."""
        options = [(w, v) for w, v in options if w > 0]
        if not options:
            return None
        total = sum(w for w, _ in options)
        x = self.r.random() * total
        for w, v in options:
            x -= w
            if x <= 0:
                return v
        return options[-1][1]

    def sym(self):
        return self.r.randrange(len(self.p.sizes))

    def val(self, s):
        return self.p.sizes[s]

    # -- types
    def gen_type(self, allow_tuple=True, allow_int=True, depth=0,
                 force=None):
        w = self.w
        opts = [(w("real"), "real"), (w("vector"), "vector"),
                (w("row_vector"), "row_vector"), (w("matrix"), "matrix"),
                (w("array_real"), "array_real"),
                (w("array_vector"), "array_vector")]
        if allow_int:
            opts += [(w("int"), "int"), (w("array_int"), "array_int")]
        if allow_tuple and depth == 0:
            opts.append((w("tuple"), "tuple"))
        k = force or self.pick(opts) or "real"
        if k == "real":
            return REAL
        if k == "int":
            return INT
        if k in ("vector", "row_vector"):
            return T(k, (self.sym(),))
        if k == "matrix":
            return mat(self.sym(), self.sym())
        if k == "array_real":
            return arr(self.sym(), REAL)
        if k == "array_int":
            return arr(self.sym(), INT)
        if k == "array_vector":
            return arr(self.sym(), vec(self.sym()))
        items = [self.gen_type(False, allow_int, 1) for _ in range(2)]
        if self.chance("tuple_nested", 1.0):
            items[1] = tup(self.gen_type(False, allow_int, 1),
                           self.gen_type(False, allow_int, 1))
        return tup(*items)

    # -- literals and simple pieces
    def lit(self, t):
        if t == INT:
            return Lit(INT, self.r.randrange(4))
        return Lit(t, round(self.r.uniform(-1.0, 1.5), 2))

    def real_lit(self, lo=-1.0, hi=1.5):
        return Lit(REAL, round(self.r.uniform(lo, hi), 2))

    def scale(self, ctx, d):
        """A positive scale, exp(tanh(x))."""
        return Call("exp", [Call("tanh", [self.expr(REAL, ctx, d)], REAL)],
                    REAL)

    def size(self, s, ctx, d=1):
        """An int expression with the value sizes[s]."""
        opts = [(1.0, "lit")]
        if ctx.fn is None:
            opts.append((2.0, "data"))
        vias = [v for v in ctx.scope if size_how(v.t, s)]
        if vias:
            opts.append((2.0, "var"))
        if d > 0:
            opts.append((self.w("call_in_size"), "call"))
        how = self.pick(opts)
        if how in ("lit", "data"):
            return Size(s, how)
        if how == "var":
            v = self.r.choice(vias)
            return Size(s, size_how(v.t, s), Var(v.name, v.t))
        t = self.r.choice([vec(s), rvec(s), arr(s, REAL)])
        call = self.user_call(t, ctx, d - 1)
        if call is None:
            return Size(s, "lit")
        return Size(s, size_how(t, s), call)

    def index(self, s, ctx):
        """An int expression in 1..sizes[s], or None."""
        n = self.val(s)
        opts = []
        loops = [v for v in ctx.scope if v.bound is not None and
                 v.bound <= n]
        if loops:
            opts.append((3.0, "loop"))
        if n >= 1:
            opts += [(1.5, "lit"), (0.7, "clamp")]
        if self.cfg.get("out_of_range", 0) and \
                self.r.random() < self.cfg["out_of_range"]:
            return Lit(INT, n + 1)
        how = self.pick(opts)
        if how is None:
            return None
        if how == "loop":
            v = self.r.choice(loops)
            return Var(v.name, INT)
        if how == "lit":
            return Lit(INT, self.r.randrange(1, n + 1))
        return Clamp(self.expr(INT, ctx, 1), self.size(s, ctx, 0))

    # -- expressions
    def expr(self, t, ctx, d=None):
        if d is None:
            d = self.depth
        opts = []
        vs = [v for v in ctx.scope if v.t == t]
        if vs:
            opts.append((self.w("var"), "var"))
        opts.append((self.w("literal"), "lit"))
        if d > 0:
            opts.append((self.w("call"), "call"))
            opts.append((2.5, "op"))
            opts.append((1.0, "index"))
            opts.append((0.6, "proj"))
            opts.append((self.w("ternary"), "cond"))
        for _ in range(4):
            how = self.pick(opts)
            e = None
            if how == "var":
                v = self.r.choice(vs)
                e = Var(v.name, t)
            elif how == "lit":
                e = self.lit(t)
            elif how == "call":
                e = self.user_call(t, ctx, d - 1)
            elif how == "op":
                e = self.compound(t, ctx, d - 1)
            elif how == "index":
                e = self.index_into(t, ctx)
            elif how == "proj":
                e = self.proj(t, ctx, d - 1)
            elif how == "cond":
                e = Cond(self.cond(ctx, d - 1), self.expr(t, ctx, d - 1),
                         self.expr(t, ctx, d - 1), t)
            if e is not None:
                return e
        return self.lit(t)

    def compound(self, t, ctx, d):
        k = t.kind
        r = self.r
        if k == "real":
            how = self.pick([(3, "arith"), (1, "intmul"), (2, "unary"),
                             (1, "exptanh"), (0.7, "div"), (1.5, "reduce"),
                             (0.8, "density"), (1.0, "rng"),
                             (0.5, "dot")])
            if how == "arith":
                return Op(r.choice("+-*"), self.expr(REAL, ctx, d),
                          self.expr(REAL, ctx, d), REAL)
            if how == "intmul":
                return Op("*", self.expr(INT, ctx, min(d, 1)),
                          self.expr(REAL, ctx, d), REAL)
            if how == "unary":
                return Call(r.choice(REAL_UNARY), [self.expr(REAL, ctx, d)],
                            REAL)
            if how == "exptanh":
                return self.scale(ctx, d)
            if how == "div":
                return Div(self.expr(REAL, ctx, d), self.expr(REAL, ctx, d),
                           REAL)
            if how == "reduce":
                c = self.gen_type(False, False)
                if c == REAL:
                    c = vec(self.sym())
                if c.kind == "array" and c.elem != REAL:
                    c = arr(c.dims[0], REAL)
                fn = "dot_self" if c.kind in ("vector", "row_vector") and \
                    r.random() < 0.4 else "sum"
                return Call(fn, [self.expr(c, ctx, d)], REAL)
            if how == "dot":
                s = self.sym()
                return Op("*", self.expr(rvec(s), ctx, d),
                          self.expr(vec(s), ctx, d), REAL)
            if how == "density":
                dist = r.choice(DENSITIES)
                y = self.expr(r.choice([REAL, vec(self.sym())]), ctx, d)
                return Call(f"{dist}_lpdf", [y, self.expr(REAL, ctx, d),
                                             self.scale(ctx, d)], REAL,
                            bar=True)
            if how == "rng" and ctx.rng and ctx.budget[0] > 0:
                ctx.budget[0] -= 1
                return Call("normal_rng", [self.expr(REAL, ctx, d),
                                           self.scale(ctx, d)], REAL)
            return None
        if k == "int":
            how = self.pick([(2, "arith"), (2, "size"), (2, "cmp"),
                             (0.5, "sum"), (0.7, "rng"), (0.5, "minmax")])
            if how == "arith":
                return Op(r.choice("+-*"), self.expr(INT, ctx, min(d, 1)),
                          self.expr(INT, ctx, min(d, 1)), INT)
            if how == "size":
                return self.size(self.sym(), ctx, d)
            if how == "cmp":
                return self.cond(ctx, d)
            if how == "sum":
                return Call("sum", [self.expr(arr(self.sym(), INT), ctx, d)],
                            INT)
            if how == "minmax":
                return Call(r.choice(["min", "max"]),
                            [self.expr(INT, ctx, d), self.expr(INT, ctx, d)],
                            INT)
            if how == "rng" and ctx.rng and ctx.budget[0] > 0:
                ctx.budget[0] -= 1
                if r.random() < 0.5:
                    return Call("bernoulli_logit_rng",
                                [self.expr(REAL, ctx, d)], INT)
                return Call("poisson_log_rng", [Call(
                    "tanh", [self.expr(REAL, ctx, d)], REAL)], INT)
            return None
        if k in ("vector", "row_vector"):
            s = t.dims[0]
            other = rvec(s) if k == "vector" else vec(s)
            how = self.pick([(2, "arith"), (1.5, "scale"), (1.5, "unary"),
                             (1, "rep"), (self.w("slice"), "slice"),
                             (self.w("multi_index"), "multi"),
                             (0.8, "matvec"), (0.7, "transpose"),
                             (0.5, "div"), (0.5, "to_vector")])
            if how == "arith":
                return Op(r.choice("+-"), self.expr(t, ctx, d),
                          self.expr(t, ctx, d), t)
            if how == "scale":
                return Op("*", self.expr(REAL, ctx, d), self.expr(t, ctx, d),
                          t)
            if how == "unary":
                return Call(r.choice(REAL_UNARY), [self.expr(t, ctx, d)], t)
            if how == "rep":
                return Call(f"rep_{k}", [self.expr(REAL, ctx, d),
                                         self.size(s, ctx, d)], t)
            if how == "slice":
                return self.slice(t, ctx)
            if how == "multi":
                return self.multi(t, ctx)
            if how == "matvec":
                c = self.sym()
                if k == "vector":
                    return Op("*", self.expr(mat(s, c), ctx, d),
                              self.expr(vec(c), ctx, d), t)
                return Op("*", self.expr(rvec(c), ctx, d),
                          self.expr(mat(c, s), ctx, d), t)
            if how == "transpose":
                return Call("'", [self.expr(other, ctx, d)], t)
            if how == "div":
                return Div(self.expr(t, ctx, d), self.expr(REAL, ctx, d), t)
            if how == "to_vector" and k == "vector":
                return Call("to_vector", [self.expr(arr(s, REAL), ctx, d)], t)
            return None
        if k == "matrix":
            how = self.pick([(2, "arith"), (1, "scale"), (1, "unary"),
                             (1, "rep")])
            if how == "arith":
                return Op(r.choice("+-"), self.expr(t, ctx, d),
                          self.expr(t, ctx, d), t)
            if how == "scale":
                return Op("*", self.expr(REAL, ctx, d), self.expr(t, ctx, d),
                          t)
            if how == "unary":
                return Call(r.choice(REAL_UNARY), [self.expr(t, ctx, d)], t)
            return Call("rep_matrix", [self.expr(REAL, ctx, d),
                                       self.size(t.dims[0], ctx, d),
                                       self.size(t.dims[1], ctx, d)], t)
        if k == "array":
            s = t.dims[0]
            how = self.pick([(1.5, "rep"), (1.5 if self.val(s) else 0, "arr"),
                             (self.w("slice"), "slice"),
                             (self.w("multi_index"), "multi"),
                             (1 if t.elem == REAL else 0, "unary")])
            if how == "rep":
                return Call("rep_array", [self.expr(t.elem, ctx, d),
                                          self.size(s, ctx, d)], t)
            if how == "arr":
                return ArrE([self.expr(t.elem, ctx, d)
                             for _ in range(self.val(s))], t)
            if how == "slice":
                return self.slice(t, ctx)
            if how == "multi":
                return self.multi(t, ctx)
            if how == "unary":
                return Call(r.choice(REAL_UNARY), [self.expr(t, ctx, d)], t)
            return None
        if k == "tuple":
            items = [None] * len(t.items)
            if ctx.budget[0] > 0 and self.chance("effect_in_tuple", 0.5):
                j = r.randrange(len(t.items))
                items[j] = self.effectful(t.items[j], ctx, d)
            return Tup([e if e is not None else self.expr(it, ctx, d)
                        for e, it in zip(items, t.items)], t)
        return None

    def effectful(self, t, ctx, d):
        """A call of type t with an effect, or None."""
        opts = []
        if ctx.rng and t in (REAL, INT):
            opts.append("rng")
        fs = [f for f in self.callable(ctx) if f.ret == t and f.effects()]
        if fs:
            opts.append("user")
        if not opts or ctx.budget[0] <= 0:
            return None
        if self.r.choice(opts) == "user":
            return self.call_of(self.r.choice(fs), ctx, d)
        ctx.budget[0] -= 1
        if t == REAL:
            return Call("normal_rng", [self.expr(REAL, ctx, d),
                                       self.scale(ctx, d)], REAL)
        return Call("poisson_log_rng", [Call(
            "tanh", [self.expr(REAL, ctx, d)], REAL)], INT)

    def slice(self, t, ctx):
        """x[1:n] of a variable whose first extent is at least n."""
        s, n = t.dims[0], self.val(t.dims[0])
        bases = [v for v in ctx.scope if v.t.kind == t.kind and
                 v.t.dims[1:] == t.dims[1:] and v.t.elem == t.elem and
                 self.val(v.t.dims[0]) >= n]
        if not bases:
            return None
        v = self.r.choice(bases)
        return Index(Var(v.name, v.t), [("upto", self.size(s, ctx, 0))], t)

    def multi(self, t, ctx):
        """x[ii] for a data index array ii."""
        if ctx.fn is not None:
            return None
        s = t.dims[0]
        bases = [v for v in ctx.scope if v.t.kind == t.kind and
                 v.t.dims[1:] == t.dims[1:] and v.t.elem == t.elem and
                 (self.val(v.t.dims[0]) >= 1 or self.val(s) == 0)]
        if not bases:
            return None
        v = self.r.choice(bases)
        ii = self.index_data(s, v.t.dims[0])
        return Index(Var(v.name, v.t), [("multi", Var(ii.name, ii.t))], t)

    def index_data(self, s, target):
        for d in self.p.datavars:
            if d.role == "index" and d.t.dims[0] == s and d.target == target:
                return d
        d = DataVar(self.name("ii"), arr(s, INT), "index", target=target)
        self.p.datavars.append(d)
        return d

    def index_into(self, t, ctx):
        """An element of a container variable, of type t."""
        cands = []
        for v in ctx.scope:
            vt = v.t
            if vt.kind == "array" and vt.elem == t:
                cands.append((v, [vt.dims[0]]))
            elif vt.kind in ("vector", "row_vector") and t == REAL:
                cands.append((v, [vt.dims[0]]))
            elif vt.kind == "matrix" and t == REAL:
                cands.append((v, [vt.dims[0], vt.dims[1]]))
            elif vt.kind == "matrix" and t == rvec(vt.dims[1]):
                cands.append((v, [vt.dims[0]]))
            elif vt.kind == "array" and vt.elem.kind in (
                    "vector", "row_vector") and t == REAL:
                cands.append((v, [vt.dims[0], vt.elem.dims[0]]))
        self.r.shuffle(cands)
        for v, syms in cands:
            idx = [self.index(s, ctx) for s in syms]
            if None in idx:
                continue
            base = Var(v.name, v.t)
            if v.t.kind == "array" and len(syms) == 2:
                inner = Index(base, [("one", idx[0])], v.t.elem)
                return Index(inner, [("one", idx[1])], t)
            return Index(base, [("one", i) for i in idx], t)
        return None

    def proj(self, t, ctx, d):
        """A tuple element of type t, from a variable or a call."""
        cands = []
        for v in ctx.scope:
            for path in tuple_paths(v.t, t):
                cands.append((Var(v.name, v.t), path))
        if not cands and d > 0 and self.r.random() < 0.5:
            fs = [f for f in self.callable(ctx) if f.ret is not None and
                  tuple_paths(f.ret, t)]
            if fs:
                f = self.r.choice(fs)
                call = self.call_of(f, ctx, d)
                if call is not None:
                    cands.append((call, self.r.choice(tuple_paths(f.ret, t))))
        if not cands:
            return None
        base, path = self.r.choice(cands)
        for k in path:
            base = Proj(base, k + 1, base.t.items[k])
        return base

    def cond(self, ctx, d):
        """An int condition, usually a comparison of reals."""
        if d > 0 and self.chance("logic", 0.5):
            return Op(self.r.choice(["&&", "||"]), self.cond(ctx, d - 1),
                      self.cond(ctx, d - 1), INT)
        if self.chance("call_in_cond", 0.5):
            a = self.user_call(REAL, ctx, d)
            if a is None:
                a = self.expr(REAL, ctx, d)
        else:
            a = self.expr(REAL, ctx, d)
        b = self.real_lit(-0.5, 0.5) if self.r.random() < 0.6 else \
            self.expr(REAL, ctx, min(d, 1))
        return Op(self.r.choice(["<", ">", "<=", ">="]), a, b, INT)

    # -- calls
    def callable(self, ctx, kinds=None):
        """Functions that ctx may call."""
        out = []
        for f in self.p.functions:
            if kinds is not None and f.kind not in kinds:
                continue
            if f.kind == "rng" and not ctx.rng:
                continue
            if f.kind == "lp" and not ctx.lp:
                continue
            if f.recursive and ctx.fn is f:
                continue
            eff = f.effects()
            if eff and ctx.budget[0] <= 0:
                continue
            if ctx.block == "td" and "reject" in eff:
                continue
            out.append(f)
        return out

    def user_call(self, t, ctx, d):
        fs = [f for f in self.callable(ctx) if f.ret == t and
              f.kind not in ("lpdf", "lpmf")]
        if t == REAL:
            fs += [f for f in self.callable(ctx, ("lpdf", "lpmf"))]
        if not fs:
            return None
        eff = [f for f in fs if f.effects()]
        if eff and self.chance("effect_call", 0.5):
            fs = eff
        return self.call_of(self.r.choice(fs), ctx, d)

    def call_of(self, f, ctx, d):
        eff = f.effects()
        if eff:
            if ctx.budget[0] <= 0:
                return None
            ctx.budget[0] -= 1
        args = self.args(f, ctx, d)
        name = f.name
        bar = f.kind in ("lpdf", "lpmf")
        if bar and ctx.lupdf and self.chance("lupdf", 0.4):
            name = name[:-5] + ("_lupdf" if f.kind == "lpdf" else "_lupmf")
        ret = f.ret if f.ret is not None else REAL
        return Call(name, args, ret, fn=f, bar=bar)

    def args(self, f, ctx, d):
        out = []
        for i, (pname, t) in enumerate(f.params):
            if f.recursive and i == 0:
                out.append(self.rec_depth(ctx))
            elif f.kind == "lpmf" and i == 0:
                out.append(self.outcome(t, ctx))
            elif t.kind == "tuple" and self.chance("tuple_arg", 0.4):
                out.append(self.compound(t, ctx, max(d, 1)))
            else:
                out.append(self.expr(t, ctx, max(d, 0)))
        return out

    def rec_depth(self, ctx):
        r = self.r.random()
        if r < 0.4:
            return Lit(INT, self.r.randrange(MAX_RECURSION))
        if r < 0.7:
            return self.size(self.sym(), ctx, 0)
        return Call("min", [self.expr(INT, ctx, 1),
                            Lit(INT, self.r.randrange(1, MAX_RECURSION))],
                    INT)

    def outcome(self, t, ctx):
        """A nonnegative int outcome (or array of them) for an _lpmf."""
        if t == INT:
            datas = [v for v in ctx.scope if v.t == INT and
                     v.name.startswith("N")]
            if datas and self.r.random() < 0.4:
                v = self.r.choice(datas)
                return Var(v.name, INT)
            if self.r.random() < 0.5:
                return self.size(self.sym(), ctx, 0)
            return Lit(INT, self.r.randrange(4))
        datas = [v for v in ctx.scope if v.t == t and v.name.startswith("d")]
        if datas and self.r.random() < 0.5:
            v = self.r.choice(datas)
            return Var(v.name, t)
        return Call("rep_array", [Lit(INT, self.r.randrange(4)),
                                  self.size(t.dims[0], ctx, 0)], t)

    # -- statements
    def stmts(self, ctx, n, depth):
        out = []
        for _ in range(n):
            out += self.stmt(ctx, depth)
        return out

    def stmt(self, ctx, depth):
        """A list of statements (a declaration may come with an assignment);
        declarations join ctx.scope."""
        w = self.w
        c = ctx.fresh()
        mutable = [v for v in ctx.scope if v.mutable]
        opts = [(w("decl"), "decl"), (w("print"), "print"),
                (w("reject"), "reject")]
        if mutable:
            opts += [(w("assign"), "assign"),
                     (w("compound_assign"), "compound"),
                     (w("indexed_assign"), "indexed"),
                     (w("tuple_assign"), "tuple_assign")]
        if depth < self.cfg["stmt_depth"]:
            opts += [(w("if"), "if"), (w("for"), "for"),
                     (w("foreach"), "foreach"), (w("while"), "while")]
            if ctx.block == "gq":
                opts.append((w("block"), "block"))
                opts.append((w("unused_draw"), "unused"))
        if ctx.in_loop:
            opts += [(w("break"), "break"), (w("continue"), "continue")]
        if ctx.fn is not None:
            opts.append((w("early_return"), "return"))
        if ctx.target:
            opts += [(w("target"), "target"), (w("tilde"), "tilde")]
        voids = [f for f in self.callable(c) if f.ret is None]
        if voids:
            opts.append((w("void_call"), "void_call"))
        for _ in range(4):
            how = self.pick(opts)
            got = self.make_stmt(how, c, ctx, depth, mutable, voids)
            if got:
                return got
        return [Print([f'"s{self.n}"'])]

    def make_stmt(self, how, c, ctx, depth, mutable, voids):
        r = self.r
        if how == "decl":
            top = ctx.fn is None and depth == 0
            t = self.gen_type(allow_int=not (top and ctx.block == "tp"))
            return [self.decl(t, c, ctx, top)]
        if how == "print":
            args = [f'"p{self.name("")}="']
            args.append(self.expr(self.gen_type(), c, 2))
            return [Print(args)]
        if how == "reject":
            return [If(self.reject_cond(c), [Reject(
                [f'"r{self.name("")}"', self.expr(REAL, c, 1)])], None)]
        if how == "assign":
            v = r.choice(mutable)
            return [Assign(Var(v.name, v.t), "=", self.expr(v.t, c))]
        if how == "compound":
            v = r.choice([m for m in mutable if m.t.kind in (
                "int", "real", "vector", "row_vector", "matrix")] or [None])
            if v is None:
                return None
            lhs = Var(v.name, v.t)
            if v.t != INT and r.random() < 0.4:
                return [Assign(lhs, "*=", Call(
                    r.choice(["tanh", "inv_logit"]),
                    [self.expr(REAL, c, 2)], REAL))]
            return [Assign(lhs, r.choice(["+=", "-="]), self.expr(v.t, c))]
        if how == "indexed":
            cands = [m for m in mutable if m.t.kind in (
                "vector", "row_vector", "array", "matrix")]
            r.shuffle(cands)
            for v in cands:
                syms = list(v.t.dims) if v.t.kind == "matrix" else \
                    [v.t.dims[0]]
                ic = c.without(v.name)
                idx = [self.index(s, ic) for s in syms]
                if None in idx:
                    continue
                et = REAL if v.t.kind != "array" else v.t.elem
                lhs = Index(Var(v.name, v.t), [("one", i) for i in idx], et)
                return [Assign(lhs, "=", self.expr(et, c))]
            return None
        if how == "tuple_assign":
            cands = [(m, path) for m in mutable if m.t.kind == "tuple"
                     for path in leaf_paths(m.t)]
            if not cands:
                return None
            v, path = r.choice(cands)
            lhs = Var(v.name, v.t)
            for k in path:
                lhs = Proj(lhs, k + 1, lhs.t.items[k])
            return [Assign(lhs, "=", self.expr(lhs.t, c))]
        if how == "if":
            cond = self.cond(c, 2)
            then = self.stmts(ctx.child(), r.randint(1, 3), depth + 1)
            els = None
            if r.random() < 0.5:
                if self.chance("else_if", 0.8):
                    inner = If(self.cond(c.fresh(), 1),
                               self.stmts(ctx.child(), r.randint(1, 2),
                                          depth + 1), None)
                    if r.random() < 0.5:
                        inner.els = self.stmts(ctx.child(), 1, depth + 1)
                    els = [inner]
                else:
                    els = self.stmts(ctx.child(), r.randint(1, 2), depth + 1)
            return [If(cond, then, els)]
        if how == "for":
            s = self.sym()
            if self.chance("call_loop_bound", 0.5):
                t = r.choice([vec(s), arr(s, REAL)])
                call = self.user_call(t, c, 1)
                hi = Size(s, size_how(t, s), call) if call is not None else \
                    self.size(s, c, 0)
            elif r.random() < 0.2:
                hi = Lit(INT, r.randint(0, 3))
            else:
                hi = self.size(s, c, 0)
            v = self.name("i")
            bound = ival(hi, self.p)
            inner = ctx.child([VarInfo(v, INT, bound=bound)], loop=True)
            body = self.stmts(inner, r.randint(1, 3), depth + 1)
            return [For(v, hi, body)]
        if how == "foreach":
            ct = r.choice([vec(self.sym()), arr(self.sym(), REAL),
                           arr(self.sym(), INT), rvec(self.sym())])
            coll = self.expr(ct, c, 1)
            et = ct.elem if ct.kind == "array" else REAL
            v = self.name("e")
            inner = ctx.child([VarInfo(v, et)], loop=True)
            body = self.stmts(inner, r.randint(1, 3), depth + 1)
            return [ForEach(v, coll, body)]
        if how == "while":
            s = self.sym()
            hi = self.size(s, c, 0) if r.random() < 0.7 else \
                Lit(INT, r.randint(0, 3))
            v = self.name("w")
            inner = ctx.child([VarInfo(v, INT, bound=ival(hi, self.p))],
                              loop=True)
            body = self.stmts(inner, r.randint(1, 3), depth + 1)
            return [While(v, hi, body)]
        if how in ("break", "continue"):
            node = Break() if how == "break" else Continue()
            return [If(self.cond(c, 1), [node], None)]
        if how == "return":
            f = ctx.fn
            ret = Return(None if f.ret is None else self.expr(f.ret, c))
            pre = []
            if r.random() < 0.3:
                pre = self.stmts(ctx.child(), 1, depth + 1)
            return [If(self.cond(c.fresh(), 1), pre + [ret], None)]
        if how == "target":
            return [Target(self.target_expr(c))]
        if how == "tilde":
            return [self.tilde(c)]
        if how == "void_call":
            f = r.choice(voids)
            call = self.call_of(f, c, 2)
            if call is None:
                return None
            call.t = None
            return [CallStmt(call)]
        if how == "block":
            inner = ctx.child()
            return [Block(self.stmts(inner, r.randint(1, 3), depth + 1))]
        if how == "unused":
            inner = ctx.child()
            c2 = inner.fresh()
            if r.random() < 0.5:
                rngs = [f for f in self.callable(c2, ("rng",))
                        if f.ret is not None]
                if rngs:
                    f = r.choice(rngs)
                    call = self.call_of(f, c2, 1)
                    if call is not None:
                        d = Decl(self.name("u"), f.ret,
                                 [self.size(s, c2, 0)
                                  for s in decl_syms(f.ret)], call)
                        return [Block([d] + self.stmts(inner, 1, depth + 1))]
            c2 = inner.fresh()
            c2.budget[0] = 0
            d = Decl(self.name("u"), REAL, [],
                     Call("normal_rng", [self.expr(REAL, c2, 1),
                                         self.scale(c2, 1)], REAL))
            return [Block([d] + self.stmts(inner, r.randint(1, 2),
                                           depth + 1))]
        return None

    def reject_cond(self, c):
        """A condition that is true for some points and data, not all."""
        reads = [v for v in c.scope if v.t == REAL]
        a = Var(self.r.choice(reads).name, REAL) if reads and \
            self.r.random() < 0.6 else self.expr(REAL, c, 1)
        return Op(">", a, self.real_lit(0.2, 1.0), INT)

    def decl(self, t, c, ctx, top=False):
        name = self.name("l" if ctx.fn is not None else
                         {"td": "td", "tp": "tp", "model": "m",
                          "gq": "gq"}[ctx.block])
        sc = c.data_only() if top else c
        sizes = [self.size(s, sc, 1) for s in decl_syms(t)]
        init = self.expr(t, c)
        ctx.scope.append(VarInfo(name, t, mutable=True,
                                 data=ctx.block == "td"))
        return Decl(name, t, sizes, init)

    def target_expr(self, c):
        r = self.r
        lps = self.callable(c, ("lpdf", "lpmf", "lp"))
        lps = [f for f in lps if f.ret is not None]
        if lps and r.random() < 0.6:
            f = r.choice(lps)
            call = self.call_of(f, c, 2)
            if call is not None:
                return call
        if r.random() < 0.3:
            dist = r.choice(DENSITIES)
            y = self.expr(r.choice([REAL, vec(self.sym())]), c, 2)
            suffix = "_lupdf" if c.lupdf and self.chance("lupdf", 0.5) \
                else "_lpdf"
            return Call(dist + suffix, [y, self.expr(REAL, c, 2),
                                        self.scale(c, 1)], REAL, bar=True)
        return self.expr(REAL, c)

    def tilde(self, c):
        r = self.r
        dists = [f for f in self.callable(c, ("lpdf", "lpmf"))]
        if dists and r.random() < 0.6:
            f = r.choice(dists)
            eff = f.effects()
            if not eff or c.budget[0] > 0:
                if eff:
                    c.budget[0] -= 1
                args = self.args(f, c, 2)
                return Tilde(args[0], f.name[:-5], args[1:], fn=f)
        dist = r.choice(DENSITIES)
        y = self.tilde_lhs(c)
        return Tilde(y, dist, [self.expr(REAL, c, 2), self.scale(c, 1)])

    def tilde_lhs(self, c):
        params = [v for v in c.scope if v.param and v.t.kind in (
            "real", "vector")]
        if params and self.r.random() < 0.7:
            v = self.r.choice(params)
            return Var(v.name, v.t)
        return self.expr(self.r.choice([REAL, vec(self.sym())]), c, 2)

    # -- functions
    def function(self, kind):
        r = self.r
        stem = self.name("f")
        suffix = {"lpdf": "_lpdf", "lpmf": "_lpmf", "rng": "_rng",
                  "lp": "_lp"}.get(kind, "")
        params = []
        if kind == "lpdf":
            params.append((self.name("y"), r.choice([REAL, vec(self.sym())])))
        elif kind == "lpmf":
            params.append((self.name("y"), r.choice(
                [INT, arr(self.sym(), INT)])))
        for _ in range(r.randint(1, 3) if kind != "lpdf" else
                       r.randint(0, 2)):
            t = self.gen_type(allow_tuple=False)
            params.append((self.name("a"), t))
        if self.w("tuple") > 0 and self.chance("tuple_arg", 0.3):
            j = r.randrange(len(params))
            if not (j == 0 and kind in ("lpdf", "lpmf")):
                params[j] = (params[j][0], self.gen_type(force="tuple"))
        if kind in ("lpdf", "lpmf"):
            ret = REAL
        elif kind == "void" or (kind == "lp" and r.random() < 0.4):
            ret = None
        else:
            ret = self.gen_type()
        f = FunDef(stem + suffix, kind, params, ret, [])
        self.fill_function(f)
        return f

    def fill_function(self, f):
        r = self.r
        ctx = Ctx(f, None, [VarInfo(n, t) for n, t in f.params])
        body = self.stmts(ctx, r.randint(*self.cfg["body_stmts"]), 0)
        if f.kind in ("lpdf", "lpmf"):
            ret = self.density_return(f, ctx.fresh())
            body.append(Return(ret))
        elif f.ret is not None:
            body.append(Return(self.expr(f.ret, ctx.fresh())))
        if f.params and self.chance("param_reuse", 0.5):
            params = Ctx(f, None, [VarInfo(n, t) for n, t in f.params])
            body.insert(0, self.reuse_guard(f, params))
        f.body = body

    def reuse_guard(self, f, c):
        """if (r(a) + r(a) > 1000) return ...; for a parameter a. It reads a
        twice before anything else and is never true."""
        tuples = [(n, t) for n, t in f.params if has_tuple(t)]
        n, t = self.r.choice(tuples if tuples and self.r.random() < 0.7
                             else f.params)
        reads = Op("+", self.reduce_to_real(Var(n, t)),
                   self.reduce_to_real(Var(n, t)), REAL)
        ret = None if f.ret is None else self.expr(f.ret, c)
        return If(Op(">", reads, Lit(REAL, 1000.0), INT), [Return(ret)],
                  None)

    def density_return(self, f, c):
        y_name, yt = f.params[0]
        y = Var(y_name, yt)
        if f.kind == "lpdf":
            dist = self.r.choice(DENSITIES)
            suffix = "_lupdf" if self.chance("lupdf", 0.3) else "_lpdf"
            core = Call(dist + suffix, [y, self.expr(REAL, c, 2),
                                        self.scale(c, 1)], REAL, bar=True)
        else:
            self.r.random()  # an unused draw that keeps existing seeds
            core = Call("poisson_log_lpmf", [y, Call(
                "tanh", [self.expr(REAL, c, 2)], REAL)], REAL, bar=True)
        if self.r.random() < 0.4:
            return Op("+", core, self.expr(REAL, c, 1), REAL)
        return core

    def recursive_function(self):
        r = self.r
        n = self.name("n")
        params = [(n, INT)] + [(self.name("a"), REAL)]
        ret = REAL
        f = FunDef(self.name("rec"), "plain", params, ret, [], recursive=True)
        ctx = Ctx(f, None, [VarInfo(p, t) for p, t in params])
        base = If(Op("<=", Var(n, INT), Lit(INT, 0), INT),
                  [Return(self.expr(REAL, ctx.fresh(), 2))], None,
                  pinned=True)
        mid = self.stmts(ctx, r.randint(0, 2), 1)
        f.body = [base] + mid
        # The self-call has f's effects, so nothing else in this statement
        # may have any.
        c = ctx.fresh()
        c.budget[0] = 0
        down = Op("-", Var(n, INT), Lit(INT, 1), INT)
        call = Call(f.name, [down, self.expr(REAL, c, 1)], REAL, fn=f)
        tail = r.choice(["+", "*"])
        ret_e = Op(tail, Call("tanh", [call], REAL), self.expr(REAL, c, 1),
                   REAL)
        f.body = [base] + mid + [Return(ret_e)]
        return f

    def overload(self, f):
        """A second definition of plain f with a different first argument."""
        r = self.r
        first = f.params[0][1]
        alt = vec(self.sym()) if first.kind != "vector" else REAL
        if first in (INT,):
            alt = vec(self.sym())
        params = [(self.name("a"), alt)] + [(self.name("a"), t)
                                            for _, t in f.params[1:]]
        g = FunDef(f.name, f.kind, params, f.ret, [])
        self.fill_function(g)
        return g

    # -- program
    def program(self):
        r, w = self.r, self.w
        lo, hi = self.cfg["sizes"]
        self.p.sizes = [r.choice(SIZE_VALUES)
                        for _ in range(r.randint(lo, hi))]
        if 0 not in self.p.sizes and r.random() < 0.3:
            self.p.sizes[r.randrange(len(self.p.sizes))] = 0
        self.p.datavars = [DataVar(f"N{s}", INT, "size", sym=s)
                           for s in range(len(self.p.sizes))]
        for _ in range(r.randint(1, 3)):
            t = self.gen_type(allow_tuple=False)
            self.p.datavars.append(DataVar(self.name("d"), t, "values"))
        nfun = r.randint(*self.cfg["functions"])
        kinds = [(w("fn_plain"), "plain"), (w("fn_void"), "void"),
                 (w("fn_lpdf"), "lpdf"), (w("fn_lpmf"), "lpmf"),
                 (w("fn_rng"), "rng"), (w("fn_lp"), "lp"),
                 (w("fn_recursive"), "recursive")]
        made = 0
        while made < nfun:
            kind = self.pick(kinds) or "plain"
            if kind == "recursive":
                f = self.recursive_function()
            else:
                f = self.function(kind)
            self.p.functions.append(f)
            made += 1
            if kind == "plain" and made < nfun and \
                    self.chance("fn_overload", 1.0):
                self.p.functions.append(self.overload(f))
                made += 1
        self.blocks()
        self.call_everything()
        return self.p

    def blocks(self):
        r = self.r
        n = self.cfg["block_stmts"]
        scope = [VarInfo(d.name, d.t, data=True) for d in self.p.datavars]
        td = Ctx(None, "td", scope)
        self.p.td = self.block_stmts(td, r.randint(0, n[1]))
        scope = scope + [VarInfo(s.name, s.t, data=True) for s in self.p.td
                         if isinstance(s, Decl)]
        self.p.params = []
        for _ in range(r.randint(1, 3)):
            t = r.choice([REAL, REAL, vec(self.sym()), rvec(self.sym()),
                          arr(self.sym(), REAL), mat(self.sym(), self.sym())])
            self.p.params.append(Param(self.name("p"), t))
        scope = scope + [VarInfo(q.name, q.t, param=True)
                         for q in self.p.params]
        tp = Ctx(None, "tp", scope)
        self.p.tp = self.block_stmts(tp, r.randint(0, n[1]))
        scope = scope + [VarInfo(s.name, s.t, param=True) for s in self.p.tp
                         if isinstance(s, Decl)]
        model = Ctx(None, "model", scope)
        self.p.model = self.block_stmts(model, r.randint(*n))
        gq = Ctx(None, "gq", scope)
        self.p.gq = self.block_stmts(gq, r.randint(*n))

    def block_stmts(self, ctx, n):
        out = []
        for _ in range(n):
            if self.r.random() < 0.4:
                t = self.gen_type(allow_int=ctx.block != "tp")
                out.append(self.decl(t, ctx.fresh(), ctx, top=True))
            else:
                out += self.stmt(ctx, 0)
        return out

    def call_everything(self):
        """Call each function that no block or later function calls yet."""
        called = set()
        for e in all_exprs(self.p):
            if isinstance(e, Call) and e.fn is not None:
                called.add(id(e.fn))
        for f in self.p.functions:
            if id(f) in called:
                continue
            if f.kind == "rng" or (f.kind in ("plain", "void") and
                                   self.r.random() < 0.6):
                block = "gq"
            elif f.kind == "plain" and self.r.random() < 0.4:
                block = "td"
            else:
                block = "model"
            body = getattr(self.p, block)
            at = self.r.randint(0, len(body))
            ctx = Ctx(None, block, self.block_scope(block, at))
            if f.kind in ("lpdf", "lpmf"):
                if self.r.random() < 0.5:
                    args = self.args(f, ctx, 2)
                    st = Tilde(args[0], f.name[:-5], args[1:], fn=f)
                else:
                    st = Target(self.call_of(f, ctx, 2))
            elif f.ret is None:
                call = self.call_of(f, ctx, 2)
                call.t = None
                st = CallStmt(call)
            else:
                call = self.call_of(f, ctx, 2)
                if block == "model" and r_true(self.r, 0.5):
                    st = Target(self.reduce_to_real(call))
                else:
                    pre = "td" if block == "td" else (
                        "m" if block == "model" else "gq")
                    sc = ctx.data_only()
                    st = Decl(self.name(pre), f.ret,
                              [self.size(s, sc, 0)
                               for s in decl_syms(f.ret)], call)
            body.insert(at, st)

    def reduce_to_real(self, e):
        t = e.t
        if t == REAL:
            return e
        if t == INT:
            return Op("*", e, Lit(REAL, 0.5), REAL)
        if t.kind in ("vector", "row_vector", "matrix") or (
                t.kind == "array" and t.elem == REAL):
            return Call("sum", [e], REAL)
        if t.kind == "tuple":
            for i, it in enumerate(t.items):
                if it in (REAL, INT):
                    return self.reduce_to_real(Proj(e, i + 1, it))
            return self.reduce_to_real(Proj(e, 1, t.items[0]))
        if t.kind == "array" and t.elem == INT:
            return Op("*", Call("sum", [e], INT), Lit(REAL, 0.5), REAL)
        if t.kind == "array":
            return Op("*", Call("size", [e], INT), Lit(REAL, 0.5), REAL)
        return Lit(REAL, 0.5)

    def block_scope(self, block, upto=None):
        """The variables visible in block before its statement `upto`."""
        p = self.p
        scope = [VarInfo(d.name, d.t, data=True) for d in p.datavars]
        scope += [VarInfo(s.name, s.t, mutable=block == "td", data=True)
                  for s in (p.td[:upto] if block == "td" else p.td)
                  if isinstance(s, Decl)]
        if block == "td":
            return scope
        scope += [VarInfo(q.name, q.t, param=True) for q in p.params]
        scope += [VarInfo(s.name, s.t, param=True, mutable=block == "tp")
                  for s in p.tp if isinstance(s, Decl)]
        if block in ("model", "gq"):
            scope += [VarInfo(s.name, s.t, mutable=True)
                      for s in getattr(p, block)[:upto]
                      if isinstance(s, Decl)]
        return scope


def r_true(r, prob):
    return r.random() < prob


def size_how(t, s):
    """How to read size s back from a value of type t, or None."""
    if t.kind == "vector" and t.dims[0] == s:
        return "rows"
    if t.kind == "row_vector" and t.dims[0] == s:
        return "cols"
    if t.kind == "array" and t.dims[0] == s:
        return "size"
    if t.kind == "matrix":
        if t.dims[0] == s:
            return "rows"
        if t.dims[1] == s:
            return "cols"
    return None


def tuple_paths(t, want, prefix=()):
    """Paths of tuple items of t that have type want."""
    out = []
    if t.kind != "tuple":
        return out
    for i, it in enumerate(t.items):
        if it == want:
            out.append(prefix + (i,))
        out += tuple_paths(it, want, prefix + (i,))
    return out


def leaf_paths(t, prefix=()):
    out = []
    for i, it in enumerate(t.items):
        if it.kind == "tuple":
            out += leaf_paths(it, prefix + (i,))
        else:
            out.append(prefix + (i,))
    return out


def generate(seed, cfg=None):
    """The program for seed under cfg (default config())."""
    return Gen(seed, cfg or config()).program()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("seed", type=int)
    ap.add_argument("--config")
    ap.add_argument("--data", action="store_true",
                    help="print the data JSON instead of the program")
    ap.add_argument("--features", action="store_true")
    args = ap.parse_args(argv)
    p = generate(args.seed, load_config(args.config))
    if args.data:
        print(json.dumps(p.data()))
    elif args.features:
        print(" ".join(p.features()))
    else:
        sys.stdout.write(p.source())
        bad = check_rules(p)
        for v in bad:
            print(f"// rule: {v}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
