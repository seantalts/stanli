#!/usr/bin/env python3
"""Shrink a generated program while it stays in the same fuzz bucket.

reduce.py regenerates the seed's program, confirms its bucket, and applies
edits greedily while the bucket stays the same: remove a function and its
calls, delete a statement, remove a variable, parameter or data variable,
drop a function argument at the definition and every call, unwrap an if or
a block, replace an expression by one of its children or by a literal,
shrink a size, and zero the data. Every candidate must keep gen.py's rules
and compile at none. It stops when no edit succeeds.

    tools/fuzz_stanc/reduce.py --seed N --out DIR [--bucket B] [--config F]
"""
from __future__ import annotations

import argparse
import concurrent.futures
import copy
import json
import pathlib
import shutil
import sys
import tempfile
import threading

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gen as g  # noqa: E402

# ---------------------------------------------------------------------------
# Slots: places that hold an expression, so an edit can replace it.


class Slot:
    def __init__(self, obj, attr, idx=None, replaceable=True):
        self.obj, self.attr, self.idx = obj, attr, idx
        self.replaceable = replaceable

    def get(self):
        v = getattr(self.obj, self.attr)
        if self.idx is None:
            return v
        v = v[self.idx]
        return v[1] if self.attr == "idx" else v

    def set(self, e):
        if self.idx is None:
            setattr(self.obj, self.attr, e)
        elif self.attr == "idx":
            kind = self.obj.idx[self.idx][0]
            self.obj.idx[self.idx] = (kind, e)
        else:
            getattr(self.obj, self.attr)[self.idx] = e


def child_slots(e):
    if isinstance(e, g.Size):
        return [Slot(e, "via")] if e.via is not None else []
    if isinstance(e, (g.Op, g.Div)):
        return [Slot(e, "a"), Slot(e, "b")]
    if isinstance(e, g.Call):
        return [Slot(e, "args", i) for i in range(len(e.args))]
    if isinstance(e, g.Cond):
        return [Slot(e, "c"), Slot(e, "a"), Slot(e, "b")]
    if isinstance(e, g.Index):
        return [Slot(e, "base", replaceable=False)] + [
            Slot(e, "idx", i) for i in range(len(e.idx))]
    if isinstance(e, g.Proj):
        return [Slot(e, "base", replaceable=False)]
    if isinstance(e, (g.Tup, g.ArrE)):
        return [Slot(e, "items", i) for i in range(len(e.items))]
    if isinstance(e, g.Clamp):
        return [Slot(e, "e"), Slot(e, "hi")]
    return []


def stmt_slots(s):
    if isinstance(s, g.Decl):
        out = [Slot(s, "sizes", i) for i in range(len(s.sizes))]
        return out + ([Slot(s, "init")] if s.init is not None else [])
    if isinstance(s, g.Assign):
        return [Slot(s, "lhs", replaceable=False), Slot(s, "rhs")]
    if isinstance(s, g.If):
        return [Slot(s, "cond")]
    if isinstance(s, (g.For, g.While)):
        return [Slot(s, "hi")]
    if isinstance(s, g.ForEach):
        return [Slot(s, "coll")]
    if isinstance(s, g.Return):
        return [Slot(s, "e")] if s.e is not None else []
    if isinstance(s, (g.Print, g.Reject)):
        return [Slot(s, "args", i) for i, a in enumerate(s.args)
                if not isinstance(a, str)]
    if isinstance(s, g.Target):
        return [Slot(s, "e")]
    if isinstance(s, g.Tilde):
        return [Slot(s, "lhs")] + [Slot(s, "args", i)
                                   for i in range(len(s.args))]
    if isinstance(s, g.CallStmt):
        return [Slot(s, "call", replaceable=False)]
    return []


def expr_slots(p):
    """Every expression slot in p, outermost first, in a fixed order."""
    out = []

    def visit(slot):
        out.append(slot)
        for c in child_slots(slot.get()):
            visit(c)

    for _, body in g.all_bodies(p):
        for s in g.walk_stmts(body):
            for slot in stmt_slots(s):
                visit(slot)
    return out


def stmt_sites(p):
    """(list, index) of every statement, in a fixed order."""
    out = []

    def visit(body):
        for i, s in enumerate(body):
            out.append((body, i))
            for b in g.stmt_bodies(s):
                visit(b)

    for _, body in g.all_bodies(p):
        visit(body)
    return out


# ---------------------------------------------------------------------------
# Edits. Each takes a copy of the program and changes it in place; it
# returns False when it does not apply.


def literal(t):
    return g.Lit(g.INT, 1) if t == g.INT else g.Lit(t, 0.5)


def root_var(e):
    while isinstance(e, (g.Index, g.Proj)):
        e = e.base
    return e.name if isinstance(e, g.Var) else None


def substitute(p, name):
    """Replace every read of variable `name` by a literal of its type."""
    def fix(slot):
        e = slot.get()
        if isinstance(e, (g.Var, g.Index, g.Proj)) and root_var(e) == name:
            if slot.replaceable:
                slot.set(literal(e.t))
                return
        if isinstance(e, g.Size) and e.via is not None and \
                g.reads(e.via, name):
            slot.set(g.Size(e.sym, "lit"))
            return
        for c in child_slots(e):
            fix(c)

    for _, body in g.all_bodies(p):
        for s in g.walk_stmts(body):
            for slot in stmt_slots(s):
                if slot.attr == "lhs" and isinstance(s, g.Assign):
                    for c in child_slots(s.lhs):
                        if c.replaceable:
                            fix(c)
                else:
                    fix(slot)


def delete_where(p, pred):
    """Delete every statement for which pred(stmt) holds."""
    def visit(body):
        body[:] = [s for s in body if not pred(s)]
        for s in body:
            for b in g.stmt_bodies(s):
                visit(b)

    for _, body in g.all_bodies(p):
        visit(body)


def remove_variable(p, name):
    """A copy of p without variable `name`: its declaration and the
    assignments to it go, and its reads become literals."""
    q = copy.deepcopy(p)
    _remove_variable(q, name)
    return q


def _remove_variable(q, name):
    delete_where(q, lambda s: (isinstance(s, g.Decl) and s.name == name) or
                 (isinstance(s, g.Assign) and root_var(s.lhs) == name))
    q.params = [x for x in q.params if x.name != name]
    q.datavars = [d for d in q.datavars if d.name != name]
    substitute(q, name)


def calls_of(p, f):
    return [e for e in g.all_exprs(p) if isinstance(e, g.Call) and e.fn is f]


def _remove_function(q, f):
    """Replace f's calls by literals and delete f."""
    delete_where(q, lambda s: (isinstance(s, g.CallStmt) and s.call.fn is f)
                 or (isinstance(s, g.Tilde) and s.fn is f))

    def fix(slot):
        e = slot.get()
        if isinstance(e, g.Call) and e.fn is f and slot.replaceable:
            slot.set(literal(e.t))
            return
        if isinstance(e, g.Size) and e.via is not None and any(
                isinstance(x, g.Call) and x.fn is f
                for x in g.walk_expr(e.via)):
            slot.set(g.Size(e.sym, "lit"))
            return
        for c in child_slots(e):
            fix(c)

    for _, body in g.all_bodies(q):
        for s in g.walk_stmts(body):
            for slot in stmt_slots(s):
                fix(slot)
    q.functions = [x for x in q.functions if x is not f]


def called(q):
    out = set()
    for where, body in g.all_bodies(q):
        for e in g.all_exprs_of(body):
            if isinstance(e, g.Call) and e.fn is not None and \
                    e.fn is not where:
                out.add(id(e.fn))
        for s in g.walk_stmts(body):
            if isinstance(s, g.Tilde) and s.fn is not None:
                out.add(id(s.fn))
    return out


def edit_prune(q, k):
    """Delete every function that nothing calls, and unused data."""
    before = (len(q.functions), len(q.datavars))
    while True:
        used = called(q)
        keep = [f for f in q.functions if id(f) in used]
        if len(keep) == len(q.functions):
            break
        q.functions = keep
    reads = {e.name for e in g.all_exprs(q) if isinstance(e, g.Var)}
    q.datavars = [d for d in q.datavars if d.role == "size" or
                  d.name in reads]
    return (len(q.functions), len(q.datavars)) != before


def count_prune(p):
    return 1


def edit_function(q, k):
    _remove_function(q, q.functions[k])
    return True


def edit_stmt(q, k):
    body, i = stmt_sites(q)[k]
    s = body[i]
    if s.pinned:
        return False
    if isinstance(s, g.Return) and s.e is not None and \
            any(f.body is body and i == len(body) - 1 for f in q.functions):
        return False
    del body[i]
    return True


def decl_names(p):
    out = [s.name for _, body in g.all_bodies(p)
           for s in g.walk_stmts(body) if isinstance(s, g.Decl)]
    out += [x.name for x in p.params]
    out += [d.name for d in p.datavars if d.role != "size"]
    return out


def edit_variable(q, k):
    _remove_variable(q, decl_names(q)[k])
    return True


def arg_sites(p):
    out = []
    for fi, f in enumerate(p.functions):
        for j in range(len(f.params)):
            if j == 0 and (f.recursive or f.kind in ("lpdf", "lpmf")):
                continue
            out.append((fi, j))
    return out


def edit_arg(q, k):
    fi, j = arg_sites(q)[k]
    f = q.functions[fi]
    name = f.params[j][0]
    calls = calls_of(q, f)
    tildes = [s for _, body in g.all_bodies(q) for s in g.walk_stmts(body)
              if isinstance(s, g.Tilde) and s.fn is f]
    f.params.pop(j)
    substitute(q, name)
    for c in calls:
        c.args.pop(j)
    for t in tildes:
        t.args.pop(j - 1)
    return True


def edit_flatten(q, k):
    body, i = stmt_sites(q)[k // 2]
    s = body[i]
    if s.pinned:
        return False
    if isinstance(s, g.If):
        inner = s.then if k % 2 == 0 else s.els
        if inner is None:
            return False
    elif isinstance(s, g.Block) and k % 2 == 0:
        inner = s.body
    else:
        return False
    body[i:i + 1] = inner
    return True


def count_flatten(p):
    return 2 * len(stmt_sites(p))


def edit_child(q, k):
    slot = expr_slots(q)[k // 3]
    e = slot.get()
    if not slot.replaceable:
        return False
    kids = [c.get() for c in child_slots(e)
            if c.replaceable and getattr(c.get(), "t", None) == e.t]
    if k % 3 >= len(kids):
        return False
    slot.set(kids[k % 3])
    return True


def count_child(p):
    return 3 * len(expr_slots(p))


def edit_literal(q, k):
    slot = expr_slots(q)[k]
    e = slot.get()
    if not slot.replaceable or isinstance(e, g.Lit):
        return False
    t = getattr(e, "t", None)
    if t is None:
        return False
    if t == g.INT:
        v = g.ival(e, q)
        slot.set(g.Lit(g.INT, v if isinstance(v, int) else 1))
    else:
        slot.set(literal(t))
    return True


def edit_size(q, k):
    s, to_zero = divmod(k, 2)
    if q.sizes[s] == 0:
        return False
    q.sizes[s] = 0 if to_zero else q.sizes[s] - 1
    return True


def edit_data(q, k):
    d = q.datavars[k]
    if d.role != "values" or d.scale == 0:
        return False
    d.scale = 0.0
    return True


EDITS = (
    ("prune", edit_prune, count_prune),
    ("function", edit_function, lambda p: len(p.functions)),
    ("statement", edit_stmt, lambda p: len(stmt_sites(p))),
    ("variable", edit_variable, lambda p: len(decl_names(p))),
    ("argument", edit_arg, lambda p: len(arg_sites(p))),
    ("flatten", edit_flatten, count_flatten),
    ("child", edit_child, count_child),
    ("literal", edit_literal, lambda p: len(expr_slots(p))),
    ("size", edit_size, lambda p: 2 * len(p.sizes)),
    ("data", edit_data, lambda p: len(p.datavars)),
)


def candidate(cur, edit, k):
    q = copy.deepcopy(cur)
    try:
        if not edit(q, k):
            return None
    except (IndexError, AttributeError, TypeError):
        return None
    if g.check_rules(q):
        return None
    return q


def reduce(p, interesting, jobs=1, log=None):
    """The smallest program reachable from p by greedy edits for which
    interesting() stays true. p is not changed."""
    cur = copy.deepcopy(p)
    cache = {}
    pool = concurrent.futures.ThreadPoolExecutor(jobs) if jobs > 1 else None

    def test(q):
        key = (q.source(), json.dumps(q.data(), sort_keys=True))
        if key not in cache:
            cache[key] = bool(interesting(q))
        return cache[key]

    try:
        changed = True
        while changed:
            changed = False
            for name, edit, count in EDITS:
                k = 0
                while k < count(cur):
                    n = count(cur)
                    batch = []
                    j = k
                    while j < n and len(batch) < jobs:
                        q = candidate(cur, edit, j)
                        if q is not None and (q.source() != cur.source() or
                                              q.data() != cur.data()):
                            batch.append((j, q))
                        j += 1
                    if pool is None:
                        oks = [test(q) for _, q in batch]
                    else:
                        oks = list(pool.map(test, [q for _, q in batch]))
                    hit = next((i for i, ok in enumerate(oks) if ok), None)
                    if hit is None:
                        k = j
                        continue
                    k, cur = batch[hit]
                    changed = True
                    if log:
                        log(f"{name} {k}: {len(cur.source().splitlines())} "
                            "lines")
    finally:
        if pool is not None:
            pool.shutdown()
    return cur


# ---------------------------------------------------------------------------
# The real predicate: the program lands in the same bucket.


def make_ctx(args):
    import fuzz
    work = pathlib.Path(tempfile.mkdtemp(prefix="fuzz_reduce_"))
    return {"compiler": args.compiler.resolve(),
            "check": args.check.resolve(), "bench": args.bench.resolve(),
            "timeout": args.timeout,
            "work": work, "out": None, "known": [],
            "lock": threading.Lock(), "kept": {}}, fuzz


def examine(p, ctx, fuzz):
    work = pathlib.Path(tempfile.mkdtemp(dir=ctx["work"]))
    try:
        source = work / "model.stan"
        source.write_text(p.source())
        data_file = work / "data.json"
        data_file.write_text(json.dumps(p.data()))
        r = fuzz.examine({"seed": p.seed, "features": p.features(),
                          "buckets": []},
                         source, data_file, work, ctx)
        if r.get("outcome") in ("agree", "differ"):
            fuzz.retime_one(ctx, r, p.source(), p.data())
        return r
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main(argv=None):
    import fuzz
    import differential as d
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--out", type=pathlib.Path, required=True)
    ap.add_argument("--bucket", help="default: the program's first bucket")
    ap.add_argument("--config")
    ap.add_argument("--compiler", type=pathlib.Path,
                    default=fuzz.REPO / "deps" / "stanc3" / "stanli-compile")
    ap.add_argument("--check", type=pathlib.Path,
                    default=fuzz.REPO / "build-rel" / "stanli_check")
    ap.add_argument("--bench", type=pathlib.Path,
                    default=fuzz.REPO / "build-rel" / "bench_grad")
    ap.add_argument("--timeout", type=float, default=d.TIMEOUT)
    ap.add_argument("--jobs", type=int, default=24)
    args = ap.parse_args(argv)
    cfg = g.load_config(args.config)
    p = g.generate(args.seed, cfg)
    ctx, fuzz = make_ctx(args)
    try:
        first = examine(p, ctx, fuzz)
        bucket = args.bucket or (first["buckets"] or [None])[0]
        if bucket is None or bucket not in first["buckets"]:
            print(f"seed {args.seed} is not in bucket {bucket}: "
                  f"{first['buckets']}", file=sys.stderr)
            return 1

        def interesting(q):
            return bucket in examine(q, ctx, fuzz)["buckets"]

        small = reduce(p, interesting, jobs=args.jobs,
                       log=lambda m: print(m, file=sys.stderr))
        final = examine(small, ctx, fuzz)
    finally:
        shutil.rmtree(ctx["work"], ignore_errors=True)
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "reduced.stan").write_text(small.source())
    (args.out / "data.json").write_text(json.dumps(small.data()) + "\n")
    with open(args.out / "README.txt", "w") as f:
        print(f"seed {args.seed}, bucket {bucket}", file=f)
        print(f"compiler {args.compiler}", file=f)
        print(f"reduced from {len(p.source().splitlines())} to "
              f"{len(small.source().splitlines())} lines", file=f)
        print("", file=f)
        for x in final.get("diffs", []):
            print(json.dumps(x), file=f)
        for k in ("none", "o1", "work", "gradient_ns"):
            if final.get(k) is not None:
                print(f"{k}: {json.dumps(final[k])}", file=f)
    sys.stdout.write(small.source())
    return 0


if __name__ == "__main__":
    sys.exit(main())
