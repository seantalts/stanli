#!/usr/bin/env python3
"""Generate the container write/read grid replayed against CmdStan.

Issue #523: a 2-D int array filled row by row in transformed data read back
INT_MIN through every int-typed read while real reads were right. No corpus
model wrote a container that way and then read it as an int, so this grid
does it on purpose, for every container shape, write form and block.

Each block and container gets one model,
tests/function_coverage/container_flow/container_flow_<block>_<container>.stan,
with two sweeps:

  writes  every write form, read back cell by cell
  reads   every read form, on the row/prefix write form

Builtin-function writes get one model per container across all blocks.

Every cell holds a distinct value (100*i + 10*j + k, plus a real offset for
real containers) and the shapes are non-square, so a transposed or dropped
write changes some output. A read in transformed parameters or generated
quantities lands in its own output column; a read in the model block is the
coefficient of its own element of the parameter vector p, so gradient
element k is the value read. The manifest lists the label of every gradient
element so a failure names its case.

  python3 tools/gen_container_flow_models.py          # write
  python3 tools/gen_container_flow_models.py --check  # freshness gate

Record references with tools/check_function_models.py --record (see
tests/function_coverage/README.md).
"""
from __future__ import annotations

import argparse
import itertools
import json
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]
OUT_DIR = REPO / "tests/function_coverage/container_flow"
MANIFEST = REPO / "tests/function_coverage/container_flow_manifest.json"
PREFIX = "container_flow_"
W_SIZE = 1500  # larger than every int cell value, so int reads can index w


class Container:
    def __init__(self, code, base, arr, inner=None, idims=()):
        self.code, self.base, self.arr = code, base, list(arr)
        self.inner, self.idims = inner, list(idims)

    @property
    def dims(self):
        return self.arr + self.idims

    @property
    def is_int(self):
        return self.base == "int"

    def decl(self):
        return type_decl(self.base, self.arr, self.inner, self.idims)

    def unsized(self):
        inner = self.inner or self.base
        if not self.arr:
            return inner
        return "array[" + "," * (len(self.arr) - 1) + "] " + inner

    def probe(self):
        """The cell single-element reads look at; off-diagonal, in row 2."""
        return {1: (3,), 2: (2, 3), 3: (2, 1, 3), 5: (2, 1, 2, 1, 2)}[len(self.dims)]

    def cells(self):
        return list(itertools.product(*(range(1, n + 1) for n in self.dims)))


CONTAINERS = [
    Container("i1", "int", [4]),
    Container("i2", "int", [3, 4]),
    Container("i3", "int", [3, 2, 4]),
    Container("r1", "real", [4]),
    Container("r2", "real", [3, 4]),
    Container("r3", "real", [3, 2, 4]),
    Container("v", "real", [], "vector", [4]),
    Container("rv", "real", [], "row_vector", [4]),
    Container("m", "real", [], "matrix", [3, 4]),
    Container("av", "real", [3], "vector", [4]),
    Container("arv", "real", [3], "row_vector", [4]),
    Container("am", "real", [3], "matrix", [2, 4]),
    Container("avv", "real", [3, 2], "vector", [4]),
    Container("amm", "real", [2, 3, 2], "matrix", [2, 2]),
]


def type_decl(base, arr, inner, idims):
    if inner is None:
        core = base
    elif inner == "matrix":
        core = f"matrix[{idims[0]}, {idims[1]}]"
    else:
        core = f"{inner}[{idims[0]}]"
    return (f"array[{', '.join(map(str, arr))}] " if arr else "") + core


def coef(rank, d):
    """Place value of index d: 100*i + 10*j + k up to rank 3, then one
    decimal digit per index."""
    return 10 ** ((2 if rank <= 3 else rank - 1) - d)


def canonical(full):
    return sum(i * coef(len(full), d) for d, i in enumerate(full))


class View:
    """The value an index expression selects: its Stan type and, for each of
    its cells, the container cell it came from."""

    def __init__(self, c, specs):
        specs = list(specs) + [None] * (len(c.dims) - len(specs))
        self.c, self.lists, self.kept = c, [], []
        for d, (n, s) in enumerate(zip(c.dims, specs)):
            if isinstance(s, int):
                self.lists.append([s])
            else:
                self.lists.append(list(range(1, n + 1)) if s is None else list(s))
                self.kept.append(d)
        na = len(c.arr)
        arr = [len(self.lists[d]) for d in self.kept if d < na]
        ikept = [d - na for d in self.kept if d >= na]
        if c.inner is None or not ikept:
            inner, idims = None, []
        elif c.inner == "matrix" and len(ikept) == 1:
            inner = "vector" if ikept == [0] else "row_vector"
            idims = [len(self.lists[na + ikept[0]])]
        else:
            inner, idims = c.inner, [len(self.lists[na + d]) for d in ikept]
        self.arr, self.inner, self.idims = arr, inner, idims

    def decl(self):
        return type_decl(self.c.base, self.arr, self.inner, self.idims)

    def full(self, pos):
        """Container cell of view cell `pos` (one index per kept dim)."""
        it = iter(pos)
        return tuple(self.lists[d][next(it) - 1] if d in self.kept
                     else self.lists[d][0] for d in range(len(self.lists)))

    def literal(self, value):
        """Stan literal of this view, `value(full cell)` for each cell."""
        shape = [len(self.lists[d]) for d in self.kept]
        na = len(self.arr)

        def emit(prefix):
            depth = len(prefix)
            if depth < na:
                return "{" + ", ".join(emit(prefix + (i,))
                                       for i in range(1, shape[depth] + 1)) + "}"
            if self.inner is None:
                return value(self.full(prefix))
            if self.inner == "matrix":
                rows = ("[" + ", ".join(value(self.full(prefix + (r, k)))
                                        for k in range(1, shape[depth + 1] + 1)) + "]"
                        for r in range(1, shape[depth] + 1))
                return "[" + ", ".join(rows) + "]"
            row = "[" + ", ".join(value(self.full(prefix + (k,)))
                                  for k in range(1, shape[depth] + 1)) + "]"
            return row + ("'" if self.inner == "vector" else "")
        return emit(())


def idx(cell):
    return ", ".join(map(str, cell))


def value_fn(c, off, real_literal=False):
    if c.is_int:
        return lambda full: (f"{canonical(full)}.0" if real_literal
                             else str(canonical(full)))
    return lambda full: f"{canonical(full)} + {off}"


# ---- write forms -----------------------------------------------------------
# Each returns (initializer or None, statements) or a skip reason string.

STANC_REJECTS = "stanc rejects"
LIMITATION = "generator limitation"


def first_perm(n):
    return {2: [2, 1], 3: [3, 1, 2], 4: [3, 1, 4, 2]}[n]


def builtin(c, off):
    val = value_fn(c, off)
    if c.code == "i1":
        two = View(Container("", "int", [2, 2]), [])
        return "to_array_1d(" + two.literal(
            lambda f: str(canonical(((f[0] - 1) * 2 + f[1],)))) + ")"
    if c.code in ("i2", "i3"):
        return "to_int(" + View(c, []).literal(value_fn(c, off, True)) + ")"
    if c.code == "r1":
        return "to_array_1d(" + View(Container("", "real", [], "vector", [4]),
                                     []).literal(val) + ")"
    if c.code == "r2":
        return "to_array_2d(" + View(Container("", "real", [], "matrix", [3, 4]),
                                     []).literal(val) + ")"
    if c.code in ("v", "rv"):
        flat = View(Container("", "real", [4]), []).literal(val)
        return ("to_vector(" if c.code == "v" else "to_row_vector(") + flat + ")"
    if c.code == "m":
        colmajor = [val((r, k)) for k in range(1, 5) for r in range(1, 4)]
        return "to_matrix({" + ", ".join(colmajor) + "}, 3, 4)"
    return None


def write_form(form, c, x, off):
    val = value_fn(c, off)
    whole = View(c, []).literal(val)
    d0, rank = c.dims[0], len(c.dims)
    if form == "lit":
        return whole, []
    if form == "assign":
        return None, ["{", f"  {c.decl()} src = {whole};", f"  {x} = src;", "}"]
    if form == "udf":
        arg = "" if c.is_int else off
        return f"make_{c.code}({arg})", []
    if form == "builtin":
        expr = builtin(c, off)
        return (expr, []) if expr else LIMITATION + ": no builtin builds distinct cells of this type"
    if form == "loop":
        names = [f"i{d + 1}" for d in range(rank)]
        expr = " + ".join(f"{coef(rank, d)} * {n}" for d, n in enumerate(names))
        if not c.is_int:
            expr += f" + {off}"
        body = [f"{x}[{', '.join(names)}] = {expr};"]
        for d in reversed(range(rank)):
            body = [f"for ({names[d]} in 1:{c.dims[d]}) {{"] + ["  " + s for s in body] + ["}"]
        return None, body
    if form in ("row", "rowslice", "colslice"):
        if rank < 2:
            return "not applicable: rank 1 has no row"
        if form == "colslice":
            return None, [f"{x}[:, {j}] = {View(c, [None, j]).literal(val)};"
                          for j in range(1, c.dims[1] + 1)]
        tail = ", :" if form == "rowslice" else ""
        return None, [f"{x}[{i}{tail}] = {View(c, [i]).literal(val)};"
                      for i in range(1, d0 + 1)]
    if form in ("prefix2", "midslice"):
        if rank < 3:
            return "not applicable: needs rank 3 or more"
        if form == "prefix2":
            return None, [f"{x}[{i}, {j}] = {View(c, [i, j]).literal(val)};"
                          for i in range(1, d0 + 1) for j in range(1, c.dims[1] + 1)]
        return None, [f"{x}[{i}, :, {k}] = {View(c, [i, None, k]).literal(val)};"
                      for i in range(1, d0 + 1) for k in range(1, c.dims[2] + 1)]
    if form == "range":
        return None, [f"{x}[2:{d0}] = {View(c, [range(2, d0 + 1)]).literal(val)};",
                      f"{x}[1:1] = {View(c, [[1]]).literal(val)};"]
    if form == "multi":
        perm = first_perm(d0)
        return None, [f"{x}[{{{', '.join(map(str, perm))}}}] = "
                      f"{View(c, [perm]).literal(val)};"]
    if form == "cpdelem":
        return whole, [f"{x}[{idx(c.probe())}] += 5;"]
    if form == "cpdrow":
        if c.inner is None or len(c.arr) >= 2:
            return STANC_REJECTS + ": += is not defined on arrays"
        sel = [range(2, 4)] if rank == 1 else [2]
        delta = View(c, sel).literal(lambda f: str(canonical(f) % 100 + f[-1]))
        lhs = "2:3" if rank == 1 else "2"
        return whole, [f"{x}[{lhs}] += {delta};"]
    raise ValueError(form)


WRITE_FORMS = ["lit", "assign", "udf", "builtin", "loop", "row", "rowslice",
               "colslice", "prefix2", "midslice", "range", "multi", "cpdelem",
               "cpdrow"]


def read_write_form(c):
    """Sweep B writes with the form that broke in #523."""
    return "row" if len(c.dims) >= 2 else "range"


# ---- read forms ------------------------------------------------------------
# Each returns (statements, expression, is_int) or None when not applicable.

def read_form(form, c, x):
    p = c.probe()
    cell = f"{x}[{idx(p)}]"
    rank = len(c.dims)
    if form == "int":
        return ([], cell, True) if c.is_int else None
    if form == "real":
        return [f"real r = {cell};"], "r", False
    if form == "arith":
        return [], f"{cell} * 2 + 1", c.is_int
    if form == "loop":
        if not c.is_int:
            return None
        return ["int n = 0;", f"for (t in 1:{cell}) n += 1;"], "n", True
    if form == "index":
        return ([], f"w[{cell}]", False) if c.is_int else None
    if form == "gather":
        if not c.is_int:
            return None
        row = x if rank == 1 else f"{x}[{idx(p[:-1])}]"
        return [], f"dot_product(w[{row}], ramp4)", False
    if form == "size":
        return [], f"size({x})", True
    if form == "dims":
        # Stan's dims() reports a vector as N x 1 and a row_vector as 1 x N.
        width = len(c.arr) + (2 if c.inner else 0)
        return (["array[%d] int dd = dims(%s);" % (width, x),
                 "int s = 0;", "for (t in 1:size(dd)) s = s * 10 + dd[t];"],
                "s", True)
    if form == "rowcopy":
        if rank == 1:
            v = View(c, [range(2, c.dims[0] + 1)])
            return [f"{v.decl()} cp = {x}[2:{c.dims[0]}];"], f"cp[{p[0] - 1}]", c.is_int
        v = View(c, [p[0]])
        return [f"{v.decl()} cp = {x}[{p[0]}];"], f"cp[{idx(p[1:])}]", c.is_int
    if form == "colcopy":
        if rank == 1:
            return None
        v = View(c, [None, p[1]])
        rest = (p[0],) + p[2:]
        return [f"{v.decl()} cp = {x}[:, {p[1]}];"], f"cp[{idx(rest)}]", c.is_int
    raise ValueError(form)


READ_FORMS = ["int", "real", "arith", "loop", "index", "gather", "size",
              "dims", "rowcopy", "colcopy"]


# ---- blocks ----------------------------------------------------------------

class Model:
    def __init__(self, title, keep=None):
        self.title, self.keep = title, keep
        self.functions, self.tdata, self.tparams_decl = [], [], []
        self.tparams, self.model, self.gq_decl, self.gq = [], [], [], []
        self.grad = []          # label of p[k]
        self.cases = []         # manifest case records
        self.data = []          # data block declarations
        self.beta = False       # declare the vector[2] beta parameter

    def gradient_labels(self):
        return ["q"] + (["beta[1]", "beta[2]"] if self.beta else []) + self.grad

    def param(self, label):
        self.grad.append(label)
        return f"p[{len(self.grad)}]"

    def source(self):
        out = [f"// Generated by tools/gen_container_flow_models.py; do not edit.",
               f"// {self.title}"]
        if self.functions:
            out += ["functions {"] + ind(self.functions) + ["}"]
        if self.data:
            out += ["data {"] + ind(self.data) + ["}"]
        out += ["transformed data {",
                f"  vector[{W_SIZE}] w;",
                f"  for (n in 1:{W_SIZE}) w[n] = n + n * n / 1024.0;",
                "  vector[4] ramp4 = [1, 2, 3, 4]';"] + ind(self.tdata) + ["}"]
        out += ["parameters {", "  real q;"]
        if self.beta:
            out.append("  vector[2] beta;")
        if self.grad:
            out.append(f"  vector[{len(self.grad)}] p;")
        out.append("}")
        if self.tparams_decl or self.tparams:
            out += ["transformed parameters {"] + ind(self.tparams_decl + self.tparams) + ["}"]
        out += ["model {", "  q ~ std_normal();"]
        if self.beta:
            out.append("  beta ~ std_normal();")
        out += ind(self.model) + ["}"]
        if self.gq_decl or self.gq:
            out += ["generated quantities {"] + ind(self.gq_decl + self.gq) + ["}"]
        return "\n".join(out) + "\n"


BLOCK_NAMES = {"td": "transformed data block", "tp": "transformed parameters block",
               "m": "model block", "gq": "generated quantities block",
               "fn": "body of a user-defined function"}
# Real cells get a non-integer offset so an int/real mix-up shows. It is a
# parameter wherever one is in scope, so model-block writes carry gradients.
OFFSET = {"td": "0.25", "tp": "q", "m": "q", "gq": "q", "fn": "s"}


def ind(lines, n=1):
    return ["  " * n + s for s in lines]


def sink_type(is_int, site):
    return "int" if is_int and site in ("td", "gq") else "real"


def dump_cell(c, x, cell, use):
    # Int cells are read into an int scalar, the read form #523 broke.
    if c.is_int:
        return "{ int v = " + f"{x}[{idx(cell)}]; " + use("v") + " }"
    return use(f"{x}[{idx(cell)}]")


def make_helpers(model, c):
    arg = "" if c.is_int else "real s"
    model.functions += [f"{c.unsized()} make_{c.code}({arg}) {{",
                        f"  return {View(c, []).literal(value_fn(c, 's'))};", "}"]


def emit_write(blk, c, form, var):
    """(declaration, write statements) for container `var`, or a skip reason."""
    w = write_form(form, c, var, OFFSET[blk])
    if isinstance(w, str):
        return w
    init, stmts = w
    return f"{c.decl()} {var}" + (f" = {init}" if init else "") + ";", stmts


def add_case(model, blk, c, form, tag, reads):
    """One written container read at every site its block can reach.

    reads: list of (label, read-form) where read-form is 'dump' (every cell)
    or a READ_FORMS entry.
    """
    name = f"{blk}_{c.code}_{tag}"
    if model.keep and name != model.keep:
        return None
    # A function-local container must not share the function's name.
    w = emit_write(blk, c, form, "x" if blk == "fn" else name)
    if isinstance(w, str):
        return w
    decl, stmts = w
    record = {"name": name, "container": c.code, "write": form}

    def outputs(site, x, emit_out):
        """Emit every read of container `x` at `site`; emit_out(label, stmts,
        expr, is_int) places one scalar read or returns the lines."""
        lines = []
        for label, rf in reads:
            if rf == "dump":
                lines += emit_out(f"{name}__{site}_cells", None, c, x)
                continue
            r = read_form(rf, c, x)
            if r is None:
                continue
            lines += emit_out(f"{name}__{site}_{label}", r, None, x)
        return lines

    def to_target(label, r, cc, x):
        if r is None:
            lines = []
            for cell in cc.cells():
                k = model.param(f"{label}[{idx(cell)}]")
                lines.append(dump_cell(cc, x, cell, lambda v: f"target += {v} * {k};"))
            return lines
        s, e, _ = r
        k = model.param(label)
        return ["{"] + ind(s + [f"target += {e} * {k};"]) + ["}"]

    def to_column(site, decls):
        def emit(label, r, cc, x):
            if r is None:
                n = len(cc.cells())
                t = sink_type(cc.is_int, site)
                decls.append(f"array[{n}] {t} {label};")
                return [dump_cell(cc, x, cell, lambda v, k=k: f"{label}[{k + 1}] = {v};")
                        for k, cell in enumerate(cc.cells())]
            s, e, is_int = r
            decls.append(f"{sink_type(is_int, site)} {label};")
            return ["{"] + ind(s + [f"{label} = {e};"]) + ["}"]
        return emit

    if blk == "td":
        model.tdata += [decl] + stmts
        tdecl = []
        body = outputs("td", name, to_column("td", tdecl))
        model.tdata += tdecl + body
        # Copy the transformed-data results out unchanged.
        for line in tdecl:
            typ, var = line.rstrip(";").rsplit(" ", 1)
            model.gq_decl.append(f"{typ} out_{var} = {var};")
        if form == read_write_form(c) or reads[0][1] != "dump":
            model.model += outputs("m", name, to_target)
            gdecl = []
            model.gq += outputs("gq", name, to_column("gq", gdecl))
            model.gq_decl += gdecl
    elif blk == "tp":
        tdecl = []
        if c.is_int:
            body = outputs("tp", name, to_column("tp", tdecl))
            model.tparams += ["{"] + ind([decl] + stmts + body) + ["}"]
        else:
            model.tparams_decl.append(decl)
            model.tparams += stmts
            model.tparams += outputs("tp", name, to_column("tp", tdecl))
            if reads[0][1] != "dump":
                model.model += outputs("m", name, to_target)
                gdecl = []
                model.gq += outputs("gq", name, to_column("gq", gdecl))
                model.gq_decl += gdecl
        model.tparams_decl += tdecl
    elif blk == "m":
        model.model += ["{"] + ind([decl] + stmts + outputs("m", name, to_target)) + ["}"]
    elif blk == "gq":
        gdecl = []
        body = outputs("gq", name, to_column("gq", gdecl))
        model.gq_decl += [decl] + gdecl
        model.gq += stmts + body
    elif blk == "fn":
        arg = "" if c.is_int else "real s"
        model.functions += [f"{c.unsized()} {name}({arg}) {{"] + ind([decl] + stmts + ["return x;"]) + ["}"]
        call = f"{name}({'' if c.is_int else 'q'})"
        model.model += ["{"] + ind([f"{c.decl()} x = {call};"] + outputs("m", "x", to_target)) + ["}"]
        gdecl = []
        body = outputs("gq", "x", to_column("gq", gdecl))
        model.gq_decl += gdecl
        model.gq += ["{"] + ind([f"{c.decl()} x = {call};"] + body) + ["}"]
    model.cases.append(record)
    return None


def build(block, c, keep=None):
    model = Model(f"Writes and reads of {c.decl()} in the {BLOCK_NAMES[block]}.", keep)
    make_helpers(model, c)
    skipped = []
    for form in WRITE_FORMS:
        if form == "builtin":
            continue
        why = add_case(model, block, c, form, form, [("cells", "dump")])
        if why:
            skipped.append({"case": f"{block}_{c.code}_{form}", "reason": why})
    add_case(model, block, c, read_write_form(c), "reads",
             [(rf, rf) for rf in READ_FORMS])
    return model, skipped


def build_builtin(c, keep=None):
    model = Model(f"{c.decl()} built by a builtin function, in every block.", keep)
    make_helpers(model, c)
    skipped = []
    for block in BLOCK_NAMES:
        why = add_case(model, block, c, "builtin", "builtin", [("cells", "dump")])
        if why:
            skipped.append({"case": f"{block}_{c.code}_builtin", "reason": why})
    return model, skipped


# ---- run-time indices, zero sizes, variable-size locals --------------------
# These models read data, tests/function_coverage/container_flow/NAME.json.
# Writes overwrite a canonical container from `src`, which holds canonical
# values plus 1000, through indices known only at run time.

DATA = {
    "n0": 0, "n2": 2, "n3": 3,
    "k": [2, 1, 3], "perm2": [2, 1], "perm3": [3, 1, 2], "perm4": [3, 1, 4, 2],
    "empty": [],
    "G": 3, "sz": [2, 4, 1],
    # Group rows of observation indices; group 1 holds the largest index.
    "rows": [[8, 3, 0, 0], [1, 5, 7, 2], [6, 0, 0, 0]],
    "N": 8, "r": [1, 0, 2, 3, 1, 4, 0, 2], "nn": [3, 2, 4, 5, 3, 6, 2, 4],
    "X": [[1, 0.5], [1, -0.25], [1, 1.5], [1, 0.75], [1, -1], [1, 2], [1, 0.125], [1, -0.5]],
    "ind": [[1, 0, 1], [0, 0, 1], [1, 1, 0], [0, 1, 1], [1, 0, 0], [1, 1, 1]],
    "a1": [4, 7, 1, 9],
    "a2": [[110, 120, 130, 140], [210, 220, 230, 240], [310, 320, 330, 340]],
    "probs": [0.2, 0.5, 0.3],
    "beta0": [0.3, -0.2],
}
DATA_DECLS = [
    ("int<lower=0>", "n0"), ("int", "n2"), ("int", "n3"),
    ("array[3] int", "k"), ("array[2] int", "perm2"), ("array[3] int", "perm3"),
    ("array[4] int", "perm4"), ("array[0] int", "empty"),
    ("int", "G"), ("array[G] int", "sz"), ("array[G, 4] int", "rows"),
    ("int", "N"), ("array[N] int", "r"), ("array[N] int", "nn"),
    ("matrix[N, 2]", "X"), ("array[6, 3] int", "ind"), ("array[4] int", "a1"),
    ("array[3, 4] int", "a2"), ("simplex[3]", "probs"), ("vector[2]", "beta0"),
]


def unsized_arg(decl):
    """Function-argument spelling of a data declaration."""
    if decl.startswith("array["):
        dims, base = decl[6:].split("] ", 1)
        return "data array[" + "," * dims.count(",") + "] " + base.split("<")[0]
    base = decl.split("[")[0].split("<")[0]
    return "data " + {"simplex": "vector"}.get(base, base)


# Every function-block case takes all data, w and ramp4, beta and the offset.
FN_PARAMS = ([f"{unsized_arg(d)} {n}" for d, n in DATA_DECLS]
             + ["data vector w", "data vector ramp4", "vector beta", "real s"])
FN_CALL = ", ".join([n for _, n in DATA_DECLS] + ["w", "ramp4", "beta", "q"])
RNG_REJECT = STANC_REJECTS + ": _rng is allowed only in transformed data and generated quantities"


def loops(dims, body, names=None):
    """Nested loops over `dims` around body(index names) lines."""
    names = names or [f"i{d + 1}" for d in range(len(dims))]
    lines = body(names)
    for d in reversed(range(len(dims))):
        lines = [f"for ({names[d]} in 1:{dims[d]}) {{"] + ind(lines) + ["}"]
    return lines


def lin_expr(dims, names):
    """Row-major 1-based position of cell `names` in `dims`."""
    if not names:
        return "1"
    e = f"({names[0]} - 1)"
    for d, n in zip(dims[1:], names[1:]):
        e = f"({e} * {d} + {n} - 1)"
    return f"{e} + 1"


class Sink:
    """Where a case's results go in its block: output columns (td, tp, gq),
    coefficients of p in the model block, or a function's returned array."""

    def __init__(self, model, blk):
        self.model, self.blk, self.decls, self.labels = model, blk, [], []

    def _put(self, label, is_int, value, lin=None):
        if self.blk == "m":
            k = (f"p[{len(self.model.grad) + 1}]" if lin is None
                 else f"p[{self.base} + {lin}]")
            stmt = lambda v: f"target += {v} * {k};"
        elif self.blk == "fn":
            pos = len(self.labels) if lin is None else f"{self.base} + {lin}"
            stmt = lambda v: f"res[{pos}] = {v};"
        else:
            tgt = label if lin is None else f"{label}[{lin}]"
            stmt = lambda v: f"{tgt} = {v};"
        if is_int:
            return "{ int v_ = " + value + "; " + stmt("v_") + " }"
        return stmt(value)

    def scalar(self, label, expr, is_int, stmts=()):
        if self.blk == "m":
            line = self._put(label, is_int, expr)
            self.model.param(label)
        elif self.blk == "fn":
            self.labels.append(label)
            line = self._put(label, is_int, expr)
        else:
            self.decls.append(f"{sink_type(is_int, self.blk)} {label};")
            line = self._put(label, is_int, expr)
        return ["{"] + ind(list(stmts) + [line]) + ["}"]

    def cells(self, label, n, is_int, fill):
        """fill(put) returns lines; put(lin, value) stores one cell."""
        if self.blk == "m":
            self.base = len(self.model.grad)
            for t in range(1, n + 1):
                self.model.param(f"{label}[{t}]")
        elif self.blk == "fn":
            self.base = len(self.labels)
            self.labels += [f"{label}[{t}]" for t in range(1, n + 1)]
        else:
            self.decls.append(f"array[{n}] {sink_type(is_int, self.blk)} {label};")
        base = getattr(self, "base", 0)
        lines = fill(lambda lin, v: self._put(label, is_int, v, lin))
        self.base = base
        return ["{"] + ind(lines) + ["}"]


def add_dyn_case(model, blk, name, body, record):
    """body(sink, off, b) returns the case's lines; b names the beta vector."""
    if model.keep and name != model.keep:
        return None
    if record.get("rng") and blk not in ("td", "gq"):
        return RNG_REJECT
    sink = Sink(model, blk)
    off, b = OFFSET[blk], "beta0" if blk == "td" else "beta"
    lines = body(sink, off, b)
    if blk == "td":
        model.tdata += sink.decls + ["{"] + ind(lines) + ["}"]
        for line in sink.decls:
            typ, var = line.rstrip(";").rsplit(" ", 1)
            model.gq_decl.append(f"{typ} out_{var} = {var};")
    elif blk == "tp":
        model.tparams_decl += sink.decls
        model.tparams += ["{"] + ind(lines) + ["}"]
    elif blk == "m":
        model.model += ["{"] + ind(lines) + ["}"]
    elif blk == "gq":
        model.gq_decl += sink.decls
        model.gq += ["{"] + ind(lines) + ["}"]
    else:
        n = len(sink.labels)
        model.functions += ([f"array[] real {name}({', '.join(FN_PARAMS)}) {{",
                             f"  array[{n}] real res;"] + ind(lines)
                            + ["  return res;", "}"])
        model.gq_decl.append(f"array[{n}] real {name}__gq = {name}({FN_CALL});")
        if n:
            base = len(model.grad)
            for label in sink.labels:
                model.param(label)
            model.model += ["{", f"  array[{n}] real rr = {name}({FN_CALL});",
                            f"  for (t in 1:{n}) target += rr[t] * p[{base} + t];", "}"]
    model.cases.append(dict(record, name=name))
    return None


def rt_write(form, c, off):
    d0, rank = c.dims[0], len(c.dims)
    d1 = c.dims[1] if rank > 1 else None
    arg = "" if c.is_int else off
    if form == "rt_loop":
        return [f"for (i in 2:{d0}) x[i] = src[i];"]
    if form == "rt_data":
        return ["x[n2] = src[n0 + 1];"]
    if form == "rt_nested":
        return ["x[k[2]] = src[k[1]];"]
    if form == "rt_range":
        if rank == 1:
            return ["x[n2:n3] = src[n2:n3];"]
        return [f"for (g in 1:{d0}) x[g, 1:n2] = src[g, 1:n2];"]
    if form == "rt_tri":
        if rank == 1:
            return ["for (n in 1:n3) x[1:n] = src[2:(n + 1)];"]
        return [f"for (n in 1:{min(d0, d1)}) x[n, 1:n] = src[n, 1:n];"]
    if form == "rt_multi":
        return [f"x[perm{d0}] = src;"]
    if form == "rt_multi_fn":
        return [f"x[perm{d0}] = make_src_{c.code}({arg});"]
    if form == "rt_copyrange":
        return [f"x[n2:{d0}] = src[n2:{d0}];"]
    if form in ("rt_mid", "rt_col"):
        if rank < 3:
            return "not applicable: needs rank 3 or more"
        if form == "rt_mid":
            return [f"for (j in 1:{d1}) x[n2, j, 1:n2] = src[n0 + 1, j, 1:n2];"]
        return [f"for (j in 1:{d0}) x[j, :, n0 + 1] = src[j, :, n2];"]
    raise ValueError(form)


RT_FORMS = ["rt_loop", "rt_data", "rt_nested", "rt_range", "rt_tri", "rt_multi",
            "rt_multi_fn", "rt_copyrange", "rt_mid", "rt_col"]
RT_INDEX = ["k[1]", "n2", "n0 + 1", "n2", "n0 + 1"]


def src_value(c, off):
    return lambda f: (str(canonical(f) + 1000) if c.is_int
                      else f"{canonical(f) + 1000} + {off}")


def build_rt(block, c, keep=None):
    model = Model(f"Run-time-index writes and reads of {c.decl()} in the "
                  f"{BLOCK_NAMES[block]}.", keep)
    dyn_header(model)
    arg = "" if c.is_int else "real s"
    model.functions += [f"{c.unsized()} make_src_{c.code}({arg}) {{",
                        f"  return {View(c, []).literal(src_value(c, 's'))};", "}"]
    skipped = []
    dims, rank = c.dims, len(c.dims)
    for form in RT_FORMS:
        name = f"{block}_{c.code}_{form}"
        w = rt_write(form, c, OFFSET[block])
        if isinstance(w, str):
            skipped.append({"case": name, "reason": w})
            continue

        def body(sink, off, b, w=w, name=name):
            lines = [f"{c.decl()} x = {View(c, []).literal(value_fn(c, off))};",
                     f"{c.decl()} src = {View(c, []).literal(src_value(c, off))};"] + w
            n = 1
            for d in dims:
                n *= d
            lines += sink.cells(f"{name}__cells", n, c.is_int, lambda put: loops(
                dims, lambda nm: [put(lin_expr(dims, nm), f"x[{', '.join(nm)}]")]))
            lines += sink.scalar(f"{name}__rtread",
                                 f"x[{', '.join(RT_INDEX[:rank])}]", c.is_int)
            if c.is_int:
                lead = dims[:-1]
                lines += sink.cells(f"{name}__gather", max(1, _prod(lead)), False,
                                    lambda put: loops(lead, lambda nm: [put(
                                        lin_expr(lead, nm),
                                        f"dot_product(w[x{'[' + ', '.join(nm) + ']' if nm else ''}], ramp4)")]))
            return lines
        why = add_dyn_case(model, block, name, body,
                           {"container": c.code, "write": form})
        if why:
            skipped.append({"case": name, "reason": why})
    return model, skipped


def _prod(xs):
    n = 1
    for x in xs:
        n *= x
    return n


def dyn_header(model):
    model.data = [f"{d} {n};" for d, n in DATA_DECLS]
    model.beta = True


def build_mread(block, keep=None):
    """Containers assigned from a multi-index read with a data index array."""
    model = Model(f"Multi-index reads with run-time index arrays in the "
                  f"{BLOCK_NAMES[block]}.", keep)
    dyn_header(model)
    skipped = []
    for c in CONTAINERS:
        dims, rank = c.dims, len(c.dims)
        for form in ("mread_first", "mread_second"):
            name = f"{block}_{c.code}_{form}"
            if form == "mread_second" and rank < 2:
                skipped.append({"case": name, "reason": "not applicable: rank 1 has no second index"})
                continue
            sel = f"src[perm{dims[0]}]" if form == "mread_first" else f"src[:, perm{dims[1]}]"

            def body(sink, off, b, c=c, sel=sel, name=name, dims=dims):
                lines = [f"{c.decl()} src = {View(c, []).literal(src_value(c, off))};",
                         f"{c.decl()} x = {sel};"]
                return lines + sink.cells(f"{name}__cells", _prod(dims), c.is_int, lambda put: loops(
                    dims, lambda nm: [put(lin_expr(dims, nm), f"x[{', '.join(nm)}]")]))
            why = add_dyn_case(model, block, name, body, {"container": c.code, "write": form})
            if why:
                skipped.append({"case": name, "reason": why})
    return model, skipped


ZERO = [
    ("zi2", "int", ["n0", 4], None, [], "rep_array(7, n0, 4)"),
    ("zi2b", "int", [3, "n0"], None, [], "rep_array(7, 3, n0)"),
    ("zr1", "real", ["n0"], None, [], "rep_array(0.5, n0)"),
    ("zv", "real", [], "vector", ["n0"], "rep_vector(0.5, n0)"),
    ("zrv", "real", [], "row_vector", ["n0"], "rep_row_vector(0.5, n0)"),
    ("zm", "real", [], "matrix", ["n0", 4], "rep_matrix(0.5, n0, 4)"),
    ("zm2", "real", [], "matrix", [3, "n0"], "rep_matrix(0.5, 3, n0)"),
    ("zav", "real", ["n0"], "vector", [4], "rep_array(rep_vector(0.5, 4), n0)"),
    ("zav2", "real", [3], "vector", ["n0"], "rep_array(rep_vector(0.5, n0), 3)"),
]
ZERO_FORMS = ["zloop", "zassign", "zrep", "zrange", "zmulti", "zcol"]


def build_zero(block, keep=None):
    model = Model(f"Zero-size containers (extent n0 = 0 from data) in the "
                  f"{BLOCK_NAMES[block]}.", keep)
    dyn_header(model)
    skipped = []
    for code, base, arr, inner, idims, rep in ZERO:
        c = Container(code, base, arr, inner, idims)
        dims, rank = c.dims, len(c.dims)
        for form in ZERO_FORMS:
            name = f"{block}_{code}_{form}"
            if form == "zcol" and rank < 2:
                skipped.append({"case": name, "reason": "not applicable: rank 1 has no column"})
                continue

            def body(sink, off, b, c=c, form=form, name=name, rep=rep, dims=dims):
                fill = loops(dims, lambda nm: [
                    f"{{}}[{', '.join(nm)}] = "
                    + " + ".join(f"{coef(len(dims), d)} * {v}" for d, v in enumerate(nm))
                    + ("" if c.is_int else f" + {off}") + ";"])
                src = [f"{c.decl()} src;"] + [l.replace("{}", "src") for l in fill]
                lines = [f"{c.decl()} x;"]
                lines += {"zloop": [l.replace("{}", "x") for l in fill],
                          "zassign": src + ["x = src;"],
                          "zrep": [f"x = {rep};"],
                          "zrange": src + ["x[1:n0] = src[1:n0];"],
                          "zmulti": src + ["x[empty] = src[empty];"],
                          "zcol": src + ["x[:, 1:n0] = src[:, 1:n0];"]}[form]
                lines += sink.cells(f"{name}__cells", 0, c.is_int, lambda put: loops(
                    dims, lambda nm: [put(lin_expr(dims, nm), f"x[{', '.join(nm)}]")]))
                lines += sink.scalar(f"{name}__size", "size(x) * 10 + 7", True)
                lines += sink.scalar(f"{name}__numel", "num_elements(x)", True)
                lines += sink.scalar(f"{name}__dims", "enc", True, [
                    "array[size(dims(x))] int dd = dims(x);",
                    "int enc = size(dd);",
                    "for (t in 1:size(dd)) enc = enc * 10 + dd[t];"])
                return lines
            why = add_dyn_case(model, block, name, body, {"container": code, "write": form})
            if why:
                skipped.append({"case": name, "reason": why})
    return model, skipped


VARSIZE_FUNCTIONS = [
    "real comp_lpmf(array[] int rr, array[] int n, array[] int obs, matrix Xm, vector b) {",
    "  return binomial_logit_lpmf(rr[obs] | n[obs], Xm[obs] * b);",
    "}",
    "real comp_loop(array[] int obs, data array[] int rr, data matrix Xm) {",
    "  real acc = 0;",
    "  for (i in 1:size(obs)) acc += rr[obs[i]] * Xm[obs[i], 2] * i;",
    "  return acc;",
    "}",
]
SLICE = "array[sz[g]] int ix = rows[g, 1:sz[g]];"


def build_varsize(block, keep=None):
    model = Model(f"Locals whose size changes on each loop pass, in the "
                  f"{BLOCK_NAMES[block]}.", keep)
    dyn_header(model)
    model.functions += VARSIZE_FUNCTIONS
    skipped = []

    def per_group(put, lines_for_g):
        return ["for (g in 1:G) {"] + ind(lines_for_g(put)) + ["}"]

    cases = {
        "vs_int": lambda sink, off, b, nm: sink.cells(f"{nm}__int", 3, True, lambda put: per_group(
            put, lambda put: [SLICE, "int tot = 0;",
                              "for (t in 1:sz[g]) tot = tot * 10 + ix[t];", put("g", "tot")])),
        "vs_elemfill": lambda sink, off, b, nm: sink.cells(f"{nm}__int", 3, True, lambda put: per_group(
            put, lambda put: ["array[sz[g]] int ix;",
                              "for (t in 1:sz[g]) ix[t] = rows[g, t];", "int tot = 0;",
                              "for (t in 1:sz[g]) tot = tot * 10 + ix[t];", put("g", "tot")])),
        "vs_gather": lambda sink, off, b, nm: sink.cells(f"{nm}__real", 3, False, lambda put: per_group(
            put, lambda put: [SLICE, put("g", "dot_product(w[ix], linspaced_vector(sz[g], 1, sz[g]))")])),
        "vs_multi_real": lambda sink, off, b, nm: sink.cells(f"{nm}__acc", 24, False, lambda put: per_group(
            put, lambda put: [SLICE, "vector[N] acc = rep_vector(0, N);", f"acc[ix] = w[ix] + g + {off};",
                              "for (t in 1:N) " + put("(g - 1) * N + t", "acc[t]")])),
        "vs_multi_int": lambda sink, off, b, nm: sink.cells(f"{nm}__pos", 24, True, lambda put: per_group(
            put, lambda put: [SLICE, "array[N] int pos = rep_array(0, N);", "pos[ix] = rev_ix(ix);",
                              "for (t in 1:N) " + put("(g - 1) * N + t", "pos[t]")])),
        "vs_udf_comp": lambda sink, off, b, nm: sink.cells(f"{nm}__lp", 3, False, lambda put: per_group(
            put, lambda put: [SLICE, put("g", f"comp_lpmf(r | nn, ix, X, {b})")])),
        "vs_udf_slice_arg": lambda sink, off, b, nm: sink.cells(f"{nm}__lp", 3, False, lambda put: per_group(
            put, lambda put: [put("g", f"comp_lpmf(r | nn, rows[g, 1:sz[g]], X, {b})")])),
        "vs_udf_loop": lambda sink, off, b, nm: sink.cells(f"{nm}__acc", 3, False, lambda put: per_group(
            put, lambda put: [SLICE, put("g", "comp_loop(ix, r, X)")])),
    }
    model.functions += ["array[] int rev_ix(array[] int a) {", "  return reverse(a);", "}"]
    for form, fn in cases.items():
        name = f"{block}_{form}"
        why = add_dyn_case(model, block, name,
                           lambda sink, off, b, fn=fn, name=name: fn(sink, off, b, name),
                           {"container": "varsize", "write": form})
        if why:
            skipped.append({"case": name, "reason": why})
    if block == "m" and (not keep or keep == "m_vs_lupmf"):
        # The OncoBayes2 shape: a per-group likelihood term on beta.
        model.model += ["for (g in 1:G) target += comp_lupmf(r | nn, rows[g, 1:sz[g]], X, beta);"]
        model.cases.append({"name": "m_vs_lupmf", "container": "varsize", "write": "vs_lupmf"})
    return model, skipped


UDFINT_FUNCTIONS = [
    "array[] int which_elem(array[] int a, int v) {",
    "  int c = 0;",
    "  for (i in 1:size(a)) if (a[i] == v) c += 1;",
    "  array[c] int out;",
    "  int j = 1;",
    "  for (i in 1:size(a)) {",
    "    if (a[i] == v) {",
    "      out[j] = i;",
    "      j += 1;",
    "    }",
    "  }",
    "  return out;",
    "}",
    "array[] int rev_int(array[] int a) {",
    "  return reverse(a);",
    "}",
    "array[,] int tr_int(array[,] int a) {",
    "  array[size(a[1]), size(a)] int t;",
    "  for (i in 1:size(a)) for (j in 1:size(a[1])) t[j, i] = a[i, j];",
    "  return t;",
    "}",
]


def build_udfint(block, keep=None):
    model = Model(f"Int arrays returned from functions, sliced and row-selected, "
                  f"in the {BLOCK_NAMES[block]}.", keep)
    dyn_header(model)
    model.functions += UDFINT_FUNCTIONS
    skipped = []

    def dump(sink, label, x, dims):
        return sink.cells(label, _prod(dims), True, lambda put: loops(
            dims, lambda nm: [put(lin_expr(dims, nm), f"{x}[{', '.join(nm)}]")]))

    cases = {
        "which_elem_rows": (False, lambda sink, nm: [
            "array[3, 6] int pos = rep_array(0, 3, 6);",
            "for (j in 1:3) {",
            "  int c = sum(ind[:, j]);",
            "  pos[j, 1:c] = which_elem(ind[:, j], 1);",
            "}"] + dump(sink, f"{nm}__pos", "pos", [3, 6])),
        "fn_slice": (False, lambda sink, nm: [
            "array[3] int sl = rev_int(a1)[1:n3];"] + dump(sink, f"{nm}__sl", "sl", [3])),
        "fn_rowsel": (False, lambda sink, nm: [
            "array[3] int rw = tr_int(a2)[n2];"] + dump(sink, f"{nm}__rw", "rw", [3])
            + sink.scalar(f"{nm}__elem", "tr_int(a2)[4, n3]", True)),
        "rng_row": (True, lambda sink, nm: [
            "int kk = categorical_rng(probs);",
            "array[4] int row = a2[kk];"] + sink.scalar(f"{nm}__kk", "kk", True)
            + dump(sink, f"{nm}__row", "row", [4])
            + sink.scalar(f"{nm}__gather", "dot_product(w[row], ramp4)", False)),
    }
    for form, (rng, fn) in cases.items():
        name = f"{block}_{form}"
        why = add_dyn_case(model, block, name,
                           lambda sink, off, b, fn=fn, name=name: fn(sink, name),
                           {"container": "udfint", "write": form, "rng": rng})
        if why:
            skipped.append({"case": name, "reason": why})
    return model, skipped


def generate(keep=None):
    files, manifest = {}, {"models": {}, "name_count": 0, "count_label": "container cases",
                           "generator": "tools/gen_container_flow_models.py"}
    # One model per block and container (builtins: per container), because a
    # refusal fails its whole model and should hide as little as possible.
    built = {f"{b}_{c.code}": build(b, c, keep)
             for b in BLOCK_NAMES for c in CONTAINERS}
    skipped_builtins = []
    for c in CONTAINERS:
        if builtin(c, "s") is None:
            skipped_builtins += [{"case": f"{b}_{c.code}_builtin", "reason":
                                  LIMITATION + ": no builtin builds distinct cells of this type"}
                                 for b in BLOCK_NAMES]
        else:
            built[f"builtin_{c.code}"] = build_builtin(c, keep)
    for b in BLOCK_NAMES:
        for c in CONTAINERS:
            built[f"rt_{b}_{c.code}"] = build_rt(b, c, keep)
        built[f"mread_{b}"] = build_mread(b, keep)
        built[f"zero_{b}"] = build_zero(b, keep)
        built[f"varsize_{b}"] = build_varsize(b, keep)
        built[f"udfint_{b}"] = build_udfint(b, keep)
    data = json.dumps(DATA, sort_keys=True) + "\n"
    for suffix, (model, skipped) in built.items():
        name = PREFIX + suffix
        files[OUT_DIR / (name + ".stan")] = model.source()
        if model.data:
            files[OUT_DIR / (name + ".json")] = data
        manifest["models"][name] = {
            "gradient": model.gradient_labels(),
            "cases": model.cases,
            "skipped": skipped,
        }
        manifest["name_count"] += len(model.cases)
    manifest["skipped_builtins"] = skipped_builtins
    files[MANIFEST] = json.dumps(manifest, indent=0, sort_keys=True) + "\n"
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="fail if the committed files differ from the generator")
    parser.add_argument("--single", type=pathlib.Path, metavar="DIR",
                        help="also write each case as its own model in DIR, "
                             "to find which case a refusal comes from")
    args = parser.parse_args()
    files = generate()
    if args.single:
        args.single.mkdir(parents=True, exist_ok=True)
        manifest = json.loads(files[MANIFEST])
        for model in manifest["models"].values():
            for case in model["cases"]:
                for path, text in generate(case["name"]).items():
                    if path.suffix == ".stan" and f"{case['name']}" in text:
                        (args.single / (case["name"] + ".stan")).write_text(text)
                        data = path.with_suffix(".json")
                        if data in files:
                            (args.single / (case["name"] + ".json")).write_text(files[data])
    stale = [p for p, text in files.items()
             if not p.exists() or p.read_text() != text]
    if args.check:
        for p in stale:
            print(f"stale: {p.relative_to(REPO)}; run tools/gen_container_flow_models.py")
        return 1 if stale else 0
    for p, text in files.items():
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    print(f"wrote {len(files)} files ({len(stale)} changed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
