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
W_SIZE = 500  # larger than every cell value, so int reads can index w


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
        return {1: (3,), 2: (2, 3), 3: (2, 1, 3)}[len(self.dims)]

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
]


def type_decl(base, arr, inner, idims):
    if inner is None:
        core = base
    elif inner == "matrix":
        core = f"matrix[{idims[0]}, {idims[1]}]"
    else:
        core = f"{inner}[{idims[0]}]"
    return (f"array[{', '.join(map(str, arr))}] " if arr else "") + core


def canonical(full):
    return sum(i * 10 ** (2 - d) for d, i in enumerate(full))


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
    return {3: [3, 1, 2], 4: [3, 1, 4, 2]}[n]


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
        expr = " + ".join(f"{10 ** (2 - d)} * {n}" for d, n in enumerate(names))
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
        if c.inner is None:
            return STANC_REJECTS + ": += is not defined on arrays"
        sel = [range(2, 4)] if rank == 1 else [2]
        delta = View(c, sel).literal(lambda f: str(canonical(f) % 100 + f[-1]))
        lhs = "2:3" if rank == 1 else "2"
        return whole, [f"{x}[{lhs}] += {delta};"]
    raise ValueError(form)


WRITE_FORMS = ["lit", "assign", "udf", "builtin", "loop", "row", "rowslice",
               "colslice", "range", "multi", "cpdelem", "cpdrow"]


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

    def param(self, label):
        self.grad.append(label)
        return f"p[{len(self.grad)}]"

    def source(self):
        out = [f"// Generated by tools/gen_container_flow_models.py; do not edit.",
               f"// {self.title}"]
        if self.functions:
            out += ["functions {"] + ind(self.functions) + ["}"]
        out += ["transformed data {",
                f"  vector[{W_SIZE}] w;",
                f"  for (n in 1:{W_SIZE}) w[n] = n + n * n / 1024.0;",
                "  vector[4] ramp4 = [1, 2, 3, 4]';"] + ind(self.tdata) + ["}"]
        out += ["parameters {", "  real q;"]
        if self.grad:
            out.append(f"  vector[{len(self.grad)}] p;")
        out.append("}")
        if self.tparams_decl or self.tparams:
            out += ["transformed parameters {"] + ind(self.tparams_decl + self.tparams) + ["}"]
        out += ["model {", "  q ~ std_normal();"] + ind(self.model) + ["}"]
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
    for suffix, (model, skipped) in built.items():
        name = PREFIX + suffix
        files[OUT_DIR / (name + ".stan")] = model.source()
        manifest["models"][name] = {
            "gradient": ["q"] + model.grad,
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
                    if path != MANIFEST and f"{case['name']}" in text:
                        (args.single / (case["name"] + ".stan")).write_text(text)
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
