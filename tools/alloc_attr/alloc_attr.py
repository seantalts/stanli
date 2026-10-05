#!/usr/bin/env python3
"""Attribute per-gradient heap allocations to call sites.

Runs bench_grad twice per model under tools/alloc_attr/mcount.c with
different iteration counts, subtracts the two site tables (which removes
preparation and exit work), symbolizes the frames with atos and classifies
each site. The binary must be built with -g for line numbers.

  clang -O1 -g -dynamiclib tools/alloc_attr/mcount.c -o libmcount.dylib
  tools/alloc_attr/alloc_attr.py --bench build-x/bench_grad \
      --lib libmcount.dylib --models DIR [DIR ...]
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import tempfile


def run(bench, lib, model_dir, n, out):
    env = dict(os.environ, MCOUNT_OUT=out, DYLD_INSERT_LIBRARIES=lib)
    subprocess.run(
        [bench, os.path.join(model_dir, "model.tmir.sexp"),
         os.path.join(model_dir, "data.json"), str(n)],
        env=env, check=True, stdout=subprocess.DEVNULL)


def parse(path):
    total = big = 0
    images = []
    sites = []
    with open(path) as f:
        for line in f:
            p = line.split()
            if p[0] == "TOTAL":
                total, big = int(p[1]), int(p[3])
            elif p[0] == "IMAGE":
                images.append((int(p[1], 16), " ".join(p[2:])))
            elif p[0] == "SITE":
                sites.append((int(p[1]), int(p[2]), int(p[3]), int(p[4]),
                              [int(x, 16) for x in p[5:]]))
    return total, big, images, sites


CACHE = {}
FRAME = re.compile(r"^(.*) \(in ([^)]*)\)(?: \(([^:()]*(?:[^()]*)):(\d+)\))?$")


def symbolize(bench, images, sites):
    base = None
    for addr, name in images:
        if os.path.realpath(name) == os.path.realpath(bench):
            base = addr
    addrs = sorted({a for s in sites for a in s[4]})
    out = {}
    for a in addrs:
        if (a - base) in CACHE:
            out[a] = CACHE[a - base]
            continue
        lines = subprocess.run(
            ["atos", "-o", bench, "-arch", "arm64", "-l", hex(base), "-i",
             "--fullPath", hex(a)],
            capture_output=True, text=True, check=True).stdout.splitlines()
        frames = []
        for part in lines:
            m = FRAME.match(part.strip())
            if m:
                frames.append((m.group(1), m.group(3) or "",
                               int(m.group(4) or 0)))
            elif part.strip():
                frames.append((part.strip(), "", 0))
        out[a] = frames
        CACHE[a - base] = frames
    return out


def short(fn):
    fn = re.sub(r"<.*", "", fn)
    return fn.split("(")[0].split()[-1] if fn else fn


def classify(frames):
    helper = None
    for fn, f, line in frames:
        if "/Eigen/" in f or "/c++/v1/" in f or "/usr/include" in f or not f:
            continue
        if "recorder.hpp" in f:
            return ("recorder edges", f"recorder.hpp:{line} {short(fn)}")
        if "legacy.hpp" in f:
            return ("nested tape (legacy)", f"legacy.hpp:{line}")
        if "/stan/math/" in f or "/lib/boost" in f:
            if helper is None:
                helper = (f, line)
            if "/prim/prob/" in f:
                b = os.path.basename(f)
                label = f"{b}:{line}"
                if helper[0] != f:
                    label += f" (via {os.path.basename(helper[0])}:{helper[1]})"
                return ("Stan Math density temporaries", label)
            continue
        if helper is not None:
            return ("Stan Math other temporaries",
                    f"{os.path.basename(helper[0])}:{helper[1]}")
        if "executor.cpp" in f or "graph.hpp" in f:
            return ("executor", f"{os.path.basename(f)}:{line} {short(fn)}")
        if "/runtime/kernels/" in f or "/runtime/include/" in f or \
                "/runtime/src/" in f:
            return ("other kernels and runtime",
                    f"{os.path.basename(f)}:{line} {short(fn)}")
        return ("harness", f"{os.path.basename(f)}:{line}")
    if helper is not None:
        return ("Stan Math other temporaries",
                f"{os.path.basename(helper[0])}:{helper[1]}")
    return ("unattributed", short(frames[0][0]) if frames else "?")


def site_table(bench, parsed):
    total, big, images, sites = parsed
    sym = symbolize(bench, images, sites)
    table = {}
    for count, nbytes, nbig, maxsz, addrs in sites:
        frames = [fr for a in addrs for fr in sym[a]]
        key = tuple(f"{fn}@{f}:{ln}" for fn, f, ln in frames)
        cat = classify(frames)
        if not any("Executor::gradient" in fn for fn, _, _ in frames):
            cat = ("outside Executor::gradient", cat[1])
        e = table.setdefault(key, [0, 0, 0, 0, cat])
        e[0] += count
        e[1] += nbytes
        e[2] += nbig
        e[3] = max(e[3], maxsz)
    return table


def attribute(bench, lib, model_dir, small, large):
    with tempfile.TemporaryDirectory() as d:
        runs = []
        for n in (small, large):
            out = os.path.join(d, f"{n}.txt")
            run(bench, lib, model_dir, n, out)
            runs.append(site_table(bench, parse(out)))
    lo, hi = runs
    dn = large - small
    rows = collections.defaultdict(lambda: [0.0, 0.0, 0.0, 0])
    for key, (c, b, bg, mx, cat) in hi.items():
        c0, b0, bg0 = lo.get(key, [0, 0, 0, 0, None])[:3]
        if c - c0 <= 0:
            continue
        r = rows[cat]
        r[0] += (c - c0) / dn
        r[1] += (b - b0) / dn
        r[2] += (bg - bg0) / dn
        r[3] = max(r[3], mx)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", required=True)
    ap.add_argument("--lib", required=True)
    ap.add_argument("--models", nargs="+", required=True)
    ap.add_argument("--small", type=int, default=1000)
    ap.add_argument("--large", type=int, default=101000)
    ap.add_argument("--json")
    args = ap.parse_args()
    result = {}
    for m in args.models:
        name = os.path.basename(m.rstrip("/"))
        rows = attribute(os.path.abspath(args.bench), os.path.abspath(args.lib),
                         m, args.small, args.large)
        result[name] = [
            {"category": k[0], "site": k[1], "allocs": round(v[0], 3),
             "bytes": round(v[1], 1), "over64": round(v[2], 3), "max": v[3]}
            for k, v in sorted(rows.items(), key=lambda kv: -kv[1][0])]
        tot = sum(r["allocs"] for r in result[name])
        print(f"== {name}: {tot:.2f} allocations per gradient")
        for r in result[name]:
            print(f"  {r['allocs']:8.2f} {r['bytes']:9.0f}B >64:{r['over64']:6.2f} "
                  f"max {r['max']:6d}  {r['category']}: {r['site']}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(result, f, indent=1)


if __name__ == "__main__":
    sys.exit(main())
