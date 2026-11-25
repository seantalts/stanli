#!/usr/bin/env python3
"""Run the O0/O1 differential on generated programs.

Each seed's program from gen.py is compiled with --passes none and o1 and
compared in stanli by differential.py. A difference is bisected to the
first pass that causes it. Three performance checks add their own buckets:

  slow_compile   the compile that users get (no --passes) takes more than 1 s
                 and more than 10 times the none compile (timed again
                 serially before reporting)
  mir_growth     the o1 MIR is more than 8 times the size of the none MIR
  slow_gradient  a warm gradient at o1 takes more than 1.5 times, and at
                 least 100 ns more than, the unoptimized one, timed serially
                 with bench_grad. Only programs whose STANLI_PROFILE=1 report
                 at o1 shows 1.5 times the op output elements, or more calls
                 of compiled regions, are timed. A region's own work is not
                 in the element count.

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
WORK_FLOOR = 10  # elements; a few ops of difference is not a finding
GRADIENT_RATIO = 1.5
GRADIENT_FLOOR_NS = 100.0
REGION_OPS = ("OP_ISLAND", "OP_LOOP", "OP_REGION_MAP")
PERF_KINDS = ("slow_compile", "mir_growth", "slow_gradient")


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
    """Executed ops, their output elements, and calls of compiled regions in
    a STANLI_PROFILE report."""
    calls = elems = regions = 0
    for line in text.splitlines():
        if line.startswith("OP_"):
            cols = line.split()
            calls += int(cols[1])
            elems += int(cols[-1])
            if cols[0] in REGION_OPS:
                regions += int(cols[1])
    return {"calls": calls, "elems": elems, "regions": regions}


def more_work(w):
    """Whether the profile at o1 suggests more work than unoptimized."""
    if not w:
        return False
    a, b = w["none"], w["o1"]
    more_elems = a["elems"] > 0 and b["elems"] > WORK_RATIO * a["elems"] \
        and b["elems"] - a["elems"] >= WORK_FLOOR
    return more_elems or b.get("regions", 0) > a.get("regions", 0)


def slow_compile(seconds):
    """Whether the shipped compile is slow, absolutely and next to none."""
    a, b = seconds.get("none"), seconds.get("shipped")
    return bool(a and b and b > SLOW_SECONDS and b > SLOW_RATIO * a)


def perf_kinds(r):
    out = []
    if slow_compile(r.get("serial_seconds") or {
            k: r[k].get("seconds") for k in ("none", "shipped")}):
        out.append("slow_compile")
    if r["none"].get("size") and r["o1"].get("size") and \
            r["o1"]["size"] > GROWTH_RATIO * r["none"]["size"]:
        out.append("mir_growth")
    g = r.get("gradient_ns")
    if g and g["none"] and g["o1"] and \
            g["o1"] > GRADIENT_RATIO * g["none"] and \
            g["o1"] - g["none"] >= GRADIENT_FLOOR_NS:
        out.append("slow_gradient")
    return out


def error_line(text):
    """The error in a stanc message, without the warnings before it, with
    quoted names replaced by '_'."""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        low = line.lower()
        if not (low.startswith(("error", "semantic error", "syntax error"))):
            continue
        head, _, tail = line.partition(":")
        if tail.strip() and "error" in head.lower() and " in " not in head:
            msg = tail.strip()
        else:
            msg = next((x.strip() for x in lines[i + 1:] if x.strip() and
                        not re.match(r"^\s*([-|]|\d+ *[|:])", x)), line)
        return re.sub(r"'[^']*'|\"[^\"]*\"", "'_'", msg)
    return lines[0] if lines else ""


def generic(message):
    """A stanli message with generated names replaced by V."""
    return re.sub(r"\b[A-Za-z_]+\d+\w*", "V", message)


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
    for f in ("model.stan", "data.json", "none.mir", "o1.mir", "shipped.mir"):
        if (work / f).exists():
            shutil.copyfile(work / f, dest / f)
    (dest / "diffs.json").write_text(json.dumps(
        {k: record.get(k) for k in ("seed", "buckets", "guilty", "diffs",
                                    "bisect", "none", "o1", "shipped",
                                    "work", "gradient_ns")}, indent=1)
        + "\n")


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
        if none[0]["outcome"] == "unsupported":
            # stanli's compiler refuses a program that stanc accepts.
            record["outcome"] = "not_comparable"
            record["not_comparable"] = "compiler_unsupported_none"
            record["refusal"] = none[0]["message"][:300]
        else:
            record["outcome"] = "reject"
            record["reject"] = error_line(none[0]["message"]) or \
                none[0]["outcome"]
        return record
    o1 = d.evaluate(ctx, source, data_file, work, "o1", "o1", reuse=none)
    if o1 is none:
        o1 = (d.compile_model(ctx, source, "o1", work / "o1.mir"),) + none[1:]
    record["o1"] = {k: o1[0][k] for k in ("outcome", "seconds", "size",
                                          "message")}
    record["mir_identical"] = none[0]["sha"] == o1[0]["sha"]
    shipped = d.compile_model(ctx, source, None, work / "shipped.mir")
    record["shipped"] = {k: shipped[k] for k in ("outcome", "seconds",
                                                 "size")}
    tilde = "s_tilde" in record["features"]
    diffs, reason = d.differences(none, o1, tilde)
    if tilde and not reason and \
            any(k == "lp" for k, _ in d.differences(none, o1)[0]):
        record["tilde_lp_shift"] = True
    if reason:
        record["outcome"] = "not_comparable"
        record["not_comparable"] = reason
        if none[1] is not None and none[1][0] == "refused":
            record["refusal"] = none[1][1][:300]
        return record
    if o1[1] is not None and none[1] is not None:
        record["points_with_values"] = d.values_compared(none[1], o1[1])
        if not record["mir_identical"]:
            record["work"] = ad_work(ctx, source, data_file, work, none, o1)
    if diffs:
        record["diffs"] = [{"kind": k, **v} for k, v in diffs]
        guilty, steps = d.bisect(ctx, source, data_file, work, none, o1,
                                 tilde)
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


def gradient_ns(ctx, mir, data_file):
    """The fastest of five warm-gradient times from bench_grad, or None when
    the gradient fails."""
    best = None
    for _ in range(5):
        try:
            r = subprocess.run([str(ctx["bench"]), str(mir), str(data_file),
                                "2000"], capture_output=True, text=True,
                               timeout=ctx["timeout"])
        except subprocess.TimeoutExpired:
            return None
        lines = [x for x in r.stdout.splitlines() if x[:1].isdigit()]
        if r.returncode or not lines:
            return None
        ns = float(lines[-1].split()[0])
        best = ns if best is None else min(best, ns)
    return best


def retime(ctx, cfg, results, seconds=None):
    """Time the slow_compile and slow_gradient candidates again, one at a
    time, for at most `seconds`. Returns how many candidates were left
    untimed."""
    deadline = None if seconds is None else time.monotonic() + seconds
    skipped = 0
    for r in results:
        if r.get("outcome") not in ("agree", "differ") or \
                not any(retime_reasons(r)):
            continue
        if deadline is not None and time.monotonic() >= deadline:
            skipped += 1
            continue
        p = gen.generate(r["seed"], cfg)
        retime_one(ctx, r, p.source(), p.data())
    return skipped


def retime_reasons(r):
    """Whether the compile looks slow, and whether the profile suggests
    more work at o1."""
    slow = slow_compile({k: r[k]["seconds"] for k in ("none", "shipped")})
    return slow, more_work(r.get("work"))


def retime_one(ctx, r, source, data):
    slow, work = retime_reasons(r)
    if not (slow or work):
        return
    with tempfile.TemporaryDirectory(dir=ctx["work"]) as w:
        w = pathlib.Path(w)
        (w / "model.stan").write_text(source)
        (w / "data.json").write_text(json.dumps(data))
        def compile_as(name, passes):
            return d.compile_model(ctx, w / "model.stan", passes,
                                   w / f"{name}.mir", repeat=3 if slow else 1)
        runs = {"none": compile_as("none", "none")}
        if work:
            runs["o1"] = compile_as("o1", "o1")
        if slow:
            r["serial_seconds"] = {
                "none": runs["none"]["seconds"],
                "shipped": compile_as("shipped", None)["seconds"]}
        if work:
            r["gradient_ns"] = {
                k: gradient_ns(ctx, w / f"{k}.mir", w / "data.json")
                if v["outcome"] == "ok" else None for k, v in runs.items()}
        before = set(r["buckets"])
        r["buckets"] = [b for b in r["buckets"] if not b.startswith("o1:")] \
            + [f"o1:{k}" for k in perf_kinds(r)]
        r["outcome"] = "differ" if r["buckets"] else "agree"
        for b in r["buckets"]:
            if b not in before:
                keep_failure(ctx, b, r["seed"], w, r)


def summarize(results, meta, out):
    def p(line=""):
        print(line, file=out)

    n = len(results)
    secs = meta["seconds"]
    p(f"{n} programs in {secs:.1f} s ({n / secs:.1f} per second)")
    p(f"compiler {meta['compiler']}")
    if meta.get("untimed"):
        p(f"performance candidates left untimed: {meta['untimed']}")
    count = {}
    for r in results:
        count[r["outcome"]] = count.get(r["outcome"], 0) + 1
    p("outcomes: " + ", ".join(f"{k} {v}" for k, v in sorted(count.items())))
    rejects = [r for r in results if r["outcome"] == "reject"]
    p(f"generator rejects: {len(rejects)}")
    msgs = {}
    for r in rejects:
        key = r.get("reject", "")[:120]
        msgs[key] = msgs.get(key, 0) + 1
    for m, k in sorted(msgs.items(), key=lambda kv: -kv[1])[:10]:
        p(f"  {k:6d}  {m}")
    compiled = [r for r in results if "mir_identical" in r]
    p(f"different MIR at none and o1: "
      f"{sum(not r['mir_identical'] for r in compiled)} of {len(compiled)}")
    valued = [r for r in results if r.get("points_with_values")]
    p(f"programs with values at some point on both sides: {len(valued)}")
    p(f"lp shifted by a constant under ~, counted as agreeing: "
      f"{sum(1 for r in results if r.get('tilde_lp_shift'))}")
    nc = {}
    for r in results:
        if r["outcome"] == "not_comparable":
            nc[r["not_comparable"]] = nc.get(r["not_comparable"], 0) + 1
    if nc:
        p("not comparable: " + ", ".join(f"{k} {v}" for k, v in
                                         sorted(nc.items())))
        refusals = {}
        for r in results:
            if r.get("refusal"):
                key = generic(r["refusal"])[:120]
                refusals[key] = refusals.get(key, 0) + 1
        p("stanli refusals")
        for m, k in sorted(refusals.items(), key=lambda kv: -kv[1])[:15]:
            p(f"  {k:6d}  {m}")
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
    ap.add_argument("--bench", type=pathlib.Path,
                    default=REPO / "build-rel" / "bench_grad")
    ap.add_argument("--known", type=pathlib.Path, default=HERE / "known.json")
    ap.add_argument("--timeout", type=float, default=d.TIMEOUT)
    ap.add_argument("--retime-seconds", type=float,
                    help="stop the serial timing of performance candidates "
                         "after this many seconds (default: time them all)")
    args = ap.parse_args(argv)
    for tool in (args.compiler, args.check, args.bench):
        if not tool.exists():
            ap.error(f"{tool} does not exist")
    cfg = gen.load_config(args.config)
    seeds = parse_seeds(args.seeds)
    args.out.mkdir(parents=True, exist_ok=True)
    work = pathlib.Path(tempfile.mkdtemp(prefix="fuzz_gen_"))
    ctx = {"compiler": args.compiler.resolve(), "check": args.check.resolve(),
           "bench": args.bench.resolve(), "timeout": args.timeout, "work": work, "out": args.out,
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
    untimed = retime(ctx, cfg, results, args.retime_seconds)
    shutil.rmtree(work, ignore_errors=True)
    results.sort(key=lambda r: r["seed"])
    meta = {"compiler": str(args.compiler), "check": str(args.check),
            "seeds": args.seeds, "config": cfg, "seconds": seconds,
            "untimed": untimed}
    (args.out / "results.json").write_text(json.dumps(
        {**meta, "programs": results}) + "\n")
    with open(args.out / "summary.txt", "w") as f:
        summarize(results, meta, f)
    summarize(results, meta, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
