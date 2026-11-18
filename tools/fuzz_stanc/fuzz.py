#!/usr/bin/env python3
"""Run the O0/O1 differential on generated programs.

Each seed's program from gen.py is compiled with --passes none and o1 and
compared in stanli by differential.py. A difference is bisected to the
first pass that causes it. Three performance checks add their own buckets:

  slow_compile   the o1 compile takes more than 1 s and more than 10 times
                 the none compile (timed again serially before reporting)
  mir_growth     the o1 MIR is more than 8 times the size of the MIR after
                 function_inlining alone
  more_ad_work   one gradient at o1 executes more than 1.5 times the op
                 output elements that it executes unoptimized, counted by
                 stanli's STANLI_PROFILE=1 report

    tools/fuzz_stanc/fuzz.py --seeds 0:1000 --jobs 24 --out DIR
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import differential as d  # noqa: E402
import gen  # noqa: E402

KEEP_PER_BUCKET = 5
SLOW_SECONDS, SLOW_RATIO = 1.0, 10.0
GROWTH_RATIO = 8.0
WORK_RATIO = 1.5


def parse_seeds(spec):
    """"A:B" is A..B-1; commas join ranges and single seeds."""
    out = []
    for part in spec.split(","):
        if ":" in part:
            a, b = (int(x) for x in part.split(":"))
            if b < a:
                raise ValueError(f"empty seed range {part}")
            out += range(a, b)
        else:
            out.append(int(part))
    return out


def profile_work(text):
    """Executed ops and their output elements in a STANLI_PROFILE report."""
    calls = elems = 0
    for line in text.splitlines():
        if line.startswith("OP_"):
            cols = line.split()
            calls += int(cols[1])
            elems += int(cols[-1])
    return {"calls": calls, "elems": elems}


def perf_kinds(r):
    out = []
    t = r.get("serial_seconds") or {
        "none": r["none"].get("seconds"), "o1": r["o1"].get("seconds")}
    if t["none"] and t["o1"] and t["o1"] > SLOW_SECONDS and \
            t["o1"] > SLOW_RATIO * t["none"]:
        out.append("slow_compile")
    if r.get("inlined_size") and r["o1"].get("size") and \
            r["o1"]["size"] > GROWTH_RATIO * r["inlined_size"]:
        out.append("mir_growth")
    w = r.get("work")
    if w and w["none"]["elems"] > 0 and \
            w["o1"]["elems"] > WORK_RATIO * w["none"]["elems"]:
        out.append("more_ad_work")
    return out


def known_entry(bucket, diffs, known):
    text = json.dumps(diffs)
    for k in known:
        if k["bucket"] == bucket and re.search(k["match"], text):
            return k
    return None


def run_profiled(ctx, source, data_file, mir):
    env = dict(os.environ, STANLI_PROFILE="1")
    try:
        r = subprocess.run([str(ctx["check"]), str(source), str(data_file),
                            "--mir", str(mir), "--point", "0"],
                           capture_output=True, text=True, env=env,
                           timeout=ctx["timeout"])
    except subprocess.TimeoutExpired:
        return None
    return profile_work(r.stderr)


def ad_work(ctx, source, data_file, work, none, o1):
    """Profiled op counts at point 0 on both sides, or None when they are
    not comparable: no values at point 0, or a part of the model runs in
    the MIR interpreter, which the profile does not see."""
    for comp, run, engines in (none, o1):
        if run is None or run[0] != "points" or not engines or \
                engines["interpreter_fallbacks"]:
            return None
        if d.refused(run[1]["points"][0]["status"]):
            return None
    a = run_profiled(ctx, source, data_file, work / "none.mir")
    b = run_profiled(ctx, source, data_file, work / "o1.mir")
    if a is None or b is None:
        return None
    return {"none": a, "o1": b}


def keep_failure(ctx, bucket, seed, work, record):
    if ctx["out"] is None:
        return
    with ctx["lock"]:
        n = ctx["kept"].get(bucket, 0)
        if n >= KEEP_PER_BUCKET:
            return
        ctx["kept"][bucket] = n + 1
    dest = ctx["out"] / "failures" / bucket.replace(":", "__") / \
        f"seed-{seed}"
    dest.mkdir(parents=True, exist_ok=True)
    for f in ("model.stan", "data.json", "none.mir", "o1.mir"):
        if (work / f).exists():
            shutil.copyfile(work / f, dest / f)
    (dest / "diffs.json").write_text(json.dumps(
        {k: record.get(k) for k in ("seed", "buckets", "guilty", "diffs",
                                    "bisect", "none", "o1", "inlined_size",
                                    "work")}, indent=1) + "\n")


def one_program(seed, cfg, ctx):
    p = gen.generate(seed, cfg)
    record = {"seed": seed, "features": p.features(), "buckets": []}
    work = pathlib.Path(tempfile.mkdtemp(prefix=f"s{seed}_", dir=ctx["work"]))
    try:
        source = work / "model.stan"
        source.write_text(p.source())
        data_file = work / "data.json"
        data_file.write_text(json.dumps(p.data()))
        return examine(record, source, data_file, work, ctx)
    except Exception as e:  # report the seed, keep the run going
        record["outcome"] = "harness_error"
        record["error"] = repr(e)
        return record
    finally:
        shutil.rmtree(work, ignore_errors=True)


def examine(record, source, data_file, work, ctx):
    seed = record["seed"]
    none = d.evaluate(ctx, source, data_file, work, "none", "none")
    record["none"] = {k: none[0][k] for k in ("outcome", "seconds", "size")}
    if none[0]["outcome"] != "ok":
        record["outcome"] = "reject"
        record["reject"] = none[0]["message"][:400]
        return record
    o1 = d.evaluate(ctx, source, data_file, work, "o1", "o1", reuse=none)
    if o1 is none:
        o1 = (d.compile_model(ctx, source, "o1", work / "o1.mir"),) + none[1:]
    record["o1"] = {k: o1[0][k] for k in ("outcome", "seconds", "size",
                                          "message")}
    record["mir_identical"] = none[0]["sha"] == o1[0]["sha"]
    inl = d.compile_model(ctx, source, "function_inlining", work / "inl.mir")
    record["inlined_size"] = inl["size"]
    diffs, reason = d.differences(none, o1)
    if reason:
        record["outcome"] = "not_comparable"
        record["not_comparable"] = reason
        return record
    if o1[1] is not None and none[1] is not None:
        record["points_with_values"] = d.values_compared(none[1], o1[1])
        if not record["mir_identical"]:
            record["work"] = ad_work(ctx, source, data_file, work, none, o1)
    if diffs:
        record["diffs"] = [{"kind": k, **v} for k, v in diffs]
        guilty, steps = d.bisect(ctx, source, data_file, work, none, o1)
        record["guilty"] = guilty
        record["bisect"] = steps
        record["buckets"].append(d.bucket(guilty, diffs))
    record["buckets"] += [f"o1:{k}" for k in perf_kinds(record)]
    record["outcome"] = "differ" if record["buckets"] else "agree"
    known = {}
    for b in record["buckets"]:
        k = known_entry(b, record.get("diffs", []), ctx["known"])
        if k:
            known[b] = k["reason"]
    if known:
        record["known"] = known
    for b in record["buckets"]:
        keep_failure(ctx, b, seed, work, record)
    return record


def retime(ctx, cfg, results):
    """Time the slow_compile candidates again, one at a time."""
    for r in results:
        if r.get("outcome") not in ("agree", "differ"):
            continue
        t = (r["none"]["seconds"], r["o1"]["seconds"])
        if not (t[0] and t[1] and t[1] > SLOW_SECONDS and
                t[1] > SLOW_RATIO * t[0]):
            continue
        with tempfile.TemporaryDirectory(dir=ctx["work"]) as w:
            src = pathlib.Path(w) / "model.stan"
            src.write_text(gen.generate(r["seed"], cfg).source())
            r["serial_seconds"] = {
                passes: d.compile_model(ctx, src, passes,
                                        pathlib.Path(w) / "x.mir",
                                        repeat=3)["seconds"]
                for passes in ("none", "o1")}
        perf = [f"o1:{k}" for k in perf_kinds(r)]
        r["buckets"] = [b for b in r["buckets"] if not b.startswith("o1:")] \
            + perf
        r["outcome"] = "differ" if r["buckets"] else "agree"


def summarize(results, meta, out):
    def p(line=""):
        print(line, file=out)

    n = len(results)
    secs = meta["seconds"]
    p(f"{n} programs in {secs:.1f} s ({n / secs:.1f} per second)")
    p(f"compiler {meta['compiler']}")
    count = {}
    for r in results:
        count[r["outcome"]] = count.get(r["outcome"], 0) + 1
    p("outcomes: " + ", ".join(f"{k} {v}" for k, v in sorted(count.items())))
    rejects = [r for r in results if r["outcome"] == "reject"]
    p(f"generator rejects: {len(rejects)}")
    msgs = {}
    for r in rejects:
        lines = [x for x in r.get("reject", "").splitlines() if x.strip()]
        key = next((x for x in lines if x.startswith(("Error", "error"))),
                   lines[0] if lines else "")
        msgs[key[:120]] = msgs.get(key[:120], 0) + 1
    for m, k in sorted(msgs.items(), key=lambda kv: -kv[1])[:10]:
        p(f"  {k:6d}  {m}")
    compiled = [r for r in results if "mir_identical" in r]
    p(f"different MIR at none and o1: "
      f"{sum(not r['mir_identical'] for r in compiled)} of {len(compiled)}")
    valued = [r for r in results if r.get("points_with_values")]
    p(f"programs with values at some point on both sides: {len(valued)}")
    nc = {}
    for r in results:
        if r["outcome"] == "not_comparable":
            nc[r["not_comparable"]] = nc.get(r["not_comparable"], 0) + 1
    if nc:
        p("not comparable: " + ", ".join(f"{k} {v}" for k, v in
                                         sorted(nc.items())))
    new, known = {}, {}
    for r in results:
        for b in r.get("buckets", []):
            target = known if b in r.get("known", {}) else new
            target.setdefault(b, []).append(r["seed"])
    for title, table in (("new buckets", new), ("known buckets", known)):
        p()
        p(title)
        for b, seeds in sorted(table.items(), key=lambda kv: -len(kv[1])):
            p(f"  {b:45s} {len(seeds):6d}  seeds {seeds[:5]}")
    p()
    p("feature                      programs")
    feats = {}
    for r in results:
        for f in r.get("features", []):
            feats[f] = feats.get(f, 0) + 1
    for f in gen.FEATURES:
        p(f"  {f:28s} {feats.get(f, 0):6d}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seeds", required=True, help="A:B, or a comma list")
    ap.add_argument("--jobs", type=int, default=24)
    ap.add_argument("--out", type=pathlib.Path, required=True)
    ap.add_argument("--config", help="JSON merged over gen.DEFAULT_CONFIG")
    ap.add_argument("--compiler", type=pathlib.Path,
                    default=REPO / "deps" / "stanc3" / "stanli-compile")
    ap.add_argument("--check", type=pathlib.Path,
                    default=REPO / "build-rel" / "stanli_check")
    ap.add_argument("--known", type=pathlib.Path, default=HERE / "known.json")
    ap.add_argument("--timeout", type=float, default=d.TIMEOUT)
    args = ap.parse_args(argv)
    for tool in (args.compiler, args.check):
        if not tool.exists():
            ap.error(f"{tool} does not exist")
    cfg = gen.load_config(args.config)
    seeds = parse_seeds(args.seeds)
    args.out.mkdir(parents=True, exist_ok=True)
    work = pathlib.Path(tempfile.mkdtemp(prefix="fuzz_gen_"))
    ctx = {"compiler": args.compiler.resolve(), "check": args.check.resolve(),
           "timeout": args.timeout, "work": work, "out": args.out,
           "known": json.loads(args.known.read_text()),
           "lock": threading.Lock(), "kept": {}}
    started = time.monotonic()
    results = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = [pool.submit(one_program, s, cfg, ctx) for s in seeds]
        for n, f in enumerate(concurrent.futures.as_completed(futures), 1):
            results.append(f.result())
            if n % 1000 == 0:
                print(f"{n}/{len(seeds)} programs", file=sys.stderr)
    seconds = time.monotonic() - started
    retime(ctx, cfg, results)
    shutil.rmtree(work, ignore_errors=True)
    results.sort(key=lambda r: r["seed"])
    meta = {"compiler": str(args.compiler), "check": str(args.check),
            "seeds": args.seeds, "config": cfg, "seconds": seconds}
    (args.out / "results.json").write_text(json.dumps(
        {**meta, "programs": results}) + "\n")
    with open(args.out / "summary.txt", "w") as f:
        summarize(results, meta, f)
    summarize(results, meta, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
