#!/usr/bin/env python3
"""Vocabulary census of the corpus MIR: functions, densities, constraints.

  tools/hp/census.py deps/posteriordb --stanc deps/stanc3/stanc --out census.json
"""
import argparse
import collections
import concurrent.futures
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from corpus_inventory import corpus_cases  # noqa: E402
from sexp import field, parse, walk  # noqa: E402

SECTIONS = ("prepare_data", "log_prob", "generate_quantities")
PROB = re.compile(r"_(lpdf|lpmf|lupdf|lupmf|cdf|lcdf|lccdf)$")


def norm(name):
    return re.sub(r"_lu(p[dm]f)$", r"_l\1", name)


def constrain_name(node):
    if isinstance(node, str):
        return node
    return node[0]


def scan(section):
    funcs, cons, stmts = collections.Counter(), collections.Counter(), collections.Counter()
    for n in walk(section):
        if not isinstance(n, list) or not n:
            continue
        if n[0] == "StanLib" and len(n) > 1:
            funcs[n[1]] += 1
        elif n[0] == "UserDefined" and len(n) > 1:
            funcs["user:" + n[1]] += 1
        elif n[0] == "constrain" and len(n) > 1:
            cons[constrain_name(n[1])] += 1
        elif n[0] == "pattern" and len(n) == 2 and isinstance(n[1], list) and n[1]:
            stmts[n[1][0]] += 1
    return funcs, cons, stmts


def one(case, stanc, work):
    d = pathlib.Path(tempfile.mkdtemp(dir=work))
    try:
        stan = d / "model.stan"
        shutil.copyfile(case.source, stan)
        r = subprocess.run([str(stanc), "--O0", "--debug-optimized-mir", stan.name],
                           cwd=d, capture_output=True, text=True, timeout=300)
        if r.returncode:
            return case.name, None
        tree = parse(r.stdout)
        out = {}
        for s in SECTIONS + ("functions_block",):
            try:
                out[s] = scan(field(tree, s))
            except KeyError:
                out[s] = (collections.Counter(),) * 3
        return case.name, out
    finally:
        shutil.rmtree(d, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pdb", type=pathlib.Path)
    ap.add_argument("--stanc", type=pathlib.Path, required=True)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--out", type=pathlib.Path, required=True)
    a = ap.parse_args()
    cases = corpus_cases(a.pdb, "all", include_language=True)
    work = tempfile.mkdtemp()
    rows = {}
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        futs = [pool.submit(one, c, a.stanc.resolve(), work) for c in cases.values()]
        for f in futs:
            name, out = f.result()
            rows[name] = out
    shutil.rmtree(work, ignore_errors=True)
    dump = {}
    for name, out in rows.items():
        if out is None:
            dump[name] = None
            continue
        dump[name] = {s: {"funcs": dict(f), "constrain": dict(c), "stmts": dict(t)}
                      for s, (f, c, t) in out.items()}
    a.out.write_text(json.dumps({"collection": {n: c.collection for n, c in cases.items()},
                                 "models": dump}))
    print(len(cases), "models,", sum(v is None for v in dump.values()), "failed")


if __name__ == "__main__":
    main()
