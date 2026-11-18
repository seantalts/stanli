#!/usr/bin/env python3
"""Compare stanli's answers on unoptimized and optimized MIR.

Each model is compiled twice by stanli-compile, with --passes none and
--passes o1, and both MIRs run in stanli_check at the same points. The
log density, gradient, write_array rows, printed output and error messages
must agree. When they do not, the production passes are added one at a time
in the order stanc3's optimization_suite runs them, and the first pass that
moves the answer away from the unoptimized one is reported. Every step also
runs list_collapsing (see bisect_steps).

    tools/fuzz_stanc/differential.py --corpus stanc3_good --out results.json
    tools/fuzz_stanc/differential.py model.stan data.json [MODEL DATA]...
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import math
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import check_status  # noqa: E402
import check_stanc3_good as good  # noqa: E402

POINTS = (0, 1, 2)
REL_TOL = 1e-10
ABS_TOL = 1e-12
TIMEOUT = 60.0

# The production passes (Optimize.level_optimizations O1 plus stanli's
# vectorize_loops), in the order optimization_suite first applies each.
PRODUCTION_PASSES = (
    "function_inlining", "constant_propagation", "copy_propagation",
    "dead_code_elimination", "vectorize_loops", "list_collapsing",
    "optimize_soa", "allow_uninitialized_decls", "block_fixing")

# When a model differs in several ways it is bucketed by the first of these.
KINDS = ("compile_fail_o1", "compile_fail_o0", "crash", "timeout",
         "one_side_threw", "lp", "grad", "write_array", "error_message",
         "output")

STATUS_HEADS = ("OK", "EVAL_FAIL", "COMPILE_FAIL", "DATA_FAIL", "WANAMES",
                "WAVALS", "POINT")


# ---------------------------------------------------------------------------
# Comparison.


def close(a, b):
    """Do two printed doubles agree? Both NaN, or the same infinity, do."""
    a, b = float(a), float(b)
    if math.isnan(a) or math.isnan(b):
        return math.isnan(a) and math.isnan(b)
    if math.isinf(a) or math.isinf(b):
        return a == b
    return abs(a - b) <= max(ABS_TOL, REL_TOL * max(abs(a), abs(b)))


def first_value_diff(want, got):
    """(index, want, got) of the first disagreeing value, or None."""
    if len(want) != len(got):
        return ("width", len(want), len(got))
    for i, (a, b) in enumerate(zip(want, got)):
        if not close(a, b):
            return (i, a, b)
    return None


def printed_lines(text):
    """{section: [line, ...]} of the lines stanli_check did not write itself:
    the model's print statements, and reject messages that print."""
    sections, current = {"pre": []}, "pre"
    for line in text.splitlines():
        head, _, tail = line.partition(" ")
        if head == "POINT" and tail.strip().isdigit():
            current = int(tail)
            sections[current] = []
        elif head in STATUS_HEADS or line in ("WANAMES", "WAVALS"):
            continue
        else:
            sections[current].append(line)
    return sections


def refused(status):
    """Did log_prob refuse? An all-NaN row counts as a refusal."""
    return not good.accepted(status)


def compare_point(k, a, b, prints_a, prints_b):
    """[(kind, detail)] for one point. a is none, b is o1."""
    out = []
    sa, sb = a.get("status"), b.get("status")
    if sa is None or sb is None:
        if sa != sb:
            out.append(("one_side_threw", {"point": k, "none": sa, "o1": sb}))
        return out
    ra, rb = refused(sa), refused(sb)
    if ra != rb:
        out.append(("one_side_threw", {"point": k, "none": sa, "o1": sb}))
    elif ra:
        ma = sa[1] if sa[0] != "OK" else "all NaN"
        mb = sb[1] if sb[0] != "OK" else "all NaN"
        if ma != mb:
            out.append(("error_message", {
                "point": k, "none": ma, "o1": mb,
                "normalized_agree": good.normalize(ma) == good.normalize(mb)}))
    else:
        va, vb = sa[1], sb[1]
        if not close(va[0], vb[0]):
            out.append(("lp", {"point": k, "none": va[0], "o1": vb[0]}))
        diff = first_value_diff(va[1:], vb[1:])
        if diff:
            out.append(("grad", {"point": k, "index": diff[0],
                                 "none": diff[1], "o1": diff[2]}))
    wa, wb = a.get("wa"), b.get("wa")
    if wa != wb:
        if wa is None or wb is None or wa[0] != wb[0]:
            out.append(("one_side_threw", {"point": k, "write_array": True,
                                           "none": wa, "o1": wb}))
        elif wa[0] == "threw":
            out.append(("error_message", {
                "point": k, "write_array": True, "none": wa[1],
                "o1": wb[1], "normalized_agree":
                good.normalize(wa[1]) == good.normalize(wb[1])}))
        elif wa[1] != wb[1]:
            out.append(("write_array", {"point": k, "names_none": wa[1][:20],
                                        "names_o1": wb[1][:20]}))
        else:
            diff = first_value_diff(wa[2], wb[2])
            if diff:
                name = wa[1][diff[0]] if isinstance(diff[0], int) else "width"
                out.append(("write_array", {"point": k, "column": name,
                                            "none": diff[1], "o1": diff[2]}))
    if prints_a != prints_b:
        out.append(("output", {"point": k, **printed_diff(prints_a,
                                                          prints_b)}))
    return out


def printed_diff(a, b):
    """Where two lists of printed lines first differ."""
    i = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y),
             min(len(a), len(b)))
    return {"line": i, "lines_none": len(a), "lines_o1": len(b),
            "none": a[i:i + 3], "o1": b[i:i + 3]}


def run_outcome(run, records):
    """Summarize one stanli_check run: (state, payload).

    state is "timeout", "crash", "refused" (stanli cannot lower or read the
    MIR), "compile_fail" (data or model rejected), or "points"."""
    if run is None:
        return "timeout", None
    code, out, err = run
    parsed = good.parse_output(out)
    pre = parsed["pre"]
    if pre and pre[0] == "COMPILE_FAIL":
        rec = records.compile
        if rec is None or rec.get("ok", True):
            return "crash", f"no compile failure record: {pre[1][:200]}"
        if good.unsupported(rec):
            return "refused", pre[1]
        return "compile_fail", pre[1]
    if code not in (0, 1) or not parsed["points"]:
        tail = (err or out).strip().splitlines()[-1:] or [f"exit {code}"]
        return "crash", f"exit {code}: {tail[0][:200]}"
    points = {k: good.with_status(parsed["points"].get(k), records, k)
              for k in POINTS}
    return "points", {"points": points, "prints": printed_lines(out)}


def compare_runs(a, b):
    """(differences, not_comparable_reason). a is none, b is o1; each is a
    (state, payload) from run_outcome."""
    (sa, pa), (sb, pb) = a, b
    if sa == sb and sa in ("timeout", "crash", "refused"):
        return [], f"both_{sa}"
    if sa == "refused":
        return [], "stanli_refuses_none"
    for state in ("crash", "timeout"):
        if state in (sa, sb):
            return [(state, {"none": pa if sa == state else None,
                             "o1": pb if sb == state else None})], None
    if sb == "refused":
        return [("compile_fail_o1", {"stanli_refused": pb})], None
    if sa == "compile_fail" or sb == "compile_fail":
        if sa != sb:
            return [("one_side_threw", {
                "none": pa if sa == "compile_fail" else "ran",
                "o1": pb if sb == "compile_fail" else "ran"})], None
        if pa != pb:
            return [("error_message", {
                "none": pa, "o1": pb,
                "normalized_agree": good.normalize(pa) == good.normalize(pb)
            })], None
        return [], None
    diffs = []
    for k in POINTS:
        diffs += compare_point(k, pa["points"][k], pb["points"][k],
                               pa["prints"].get(k, []),
                               pb["prints"].get(k, []))
    if pa["prints"]["pre"] != pb["prints"]["pre"]:
        diffs.append(("output", {"point": "pre", **printed_diff(
            pa["prints"]["pre"], pb["prints"]["pre"])}))
    return diffs, None


def values_compared(a, b):
    """How many points returned values on both sides."""
    if a[0] != "points" or b[0] != "points":
        return 0
    return sum(1 for k in POINTS
               if not refused(a[1]["points"][k]["status"])
               and not refused(b[1]["points"][k]["status"]))


def compare_compiles(a, b):
    """(differences, not_comparable_reason) from two compile results, or
    (None, None) when both compiled."""
    oka, okb = a["outcome"] == "ok", b["outcome"] == "ok"
    if oka and okb:
        return None, None
    if not oka and not okb:
        return [], "both_compile_fail"
    if oka:
        kind = "timeout" if b["outcome"] == "timeout" else "compile_fail_o1"
        return [(kind, {"o1": b["outcome"], "message": b["message"]})], None
    if a["outcome"] == "unsupported":
        return [], "compiler_unsupported_none"
    kind = "timeout" if a["outcome"] == "timeout" else "compile_fail_o0"
    return [(kind, {"none": a["outcome"], "message": a["message"]})], None


def primary_kind(diffs):
    kinds = {k for k, _ in diffs}
    return next(k for k in KINDS if k in kinds)


def bucket(guilty, diffs):
    return f"{guilty}:{primary_kind(diffs)}"


# ---------------------------------------------------------------------------
# Running.


def run_process(argv, timeout, cwd=None):
    """(returncode, stdout, stderr, seconds), or None on timeout."""
    started = time.monotonic()
    try:
        r = subprocess.run([str(a) for a in argv], capture_output=True,
                           text=True, timeout=timeout, cwd=cwd)
    except subprocess.TimeoutExpired:
        return None
    return r.returncode, r.stdout, r.stderr, time.monotonic() - started


def compile_model(ctx, source, passes, mir_path, repeat=1):
    """The compile's outcome, MIR size and hash, and its fastest wall time
    over `repeat` runs."""
    argv = [ctx["compiler"], "--model-only", "--passes", passes,
            "--filename", source.name, source]
    run = run_process(argv, ctx["timeout"])
    for _ in range(repeat - 1):
        again = run and run_process(argv, ctx["timeout"])
        if again and again[3] < run[3]:
            run = again
    if run is None:
        return {"outcome": "timeout", "message": "", "seconds": None,
                "size": None, "sha": None}
    code, out, err, seconds = run
    result = {"seconds": seconds, "size": None, "sha": None, "message": ""}
    if code == 0 and out:
        mir_path.write_text(out)
        result.update(outcome="ok", size=len(out.encode()),
                      sha=hashlib.sha256(out.encode()).hexdigest())
        return result
    outcome, message = classify_failure(code, err)
    result.update(outcome=outcome, message=message[:400])
    return result


def classify_failure(code, err):
    """(outcome, message) of a failed compile. stanc prints its warnings
    before the error, one paragraph each, so they are dropped first."""
    paragraphs = err.strip().split("\n\n")
    while len(paragraphs) > 1 and \
            paragraphs[0].lower().startswith("warning"):
        paragraphs.pop(0)
    message = "\n\n".join(paragraphs).strip()
    if code not in (0, 1):
        return "crash", message
    if message.startswith("Internal compiler error"):
        return "internal", message
    if message.startswith(("Syntax error", "Semantic error", "Error:",
                           "error:")):
        return "frontend", message
    return "unsupported", message


def check_model(ctx, source, data_file, mir_path, status_path):
    run = run_process([ctx["check"], source, data_file, "--mir", mir_path,
                       "--points", ",".join(map(str, POINTS)), "--wa-values",
                       "--status-json", status_path], ctx["timeout"])
    records = check_status.read(status_path)
    engines = None
    if records.compile and records.compile.get("ok"):
        engines = {"interpreter_fallbacks":
                   records.compile.get("interpreter_fallbacks", []),
                   "write_array": records.write_array().get("section")}
    return run_outcome(None if run is None else run[:3], records), engines


def evaluate(ctx, source, data_file, work, passes, tag, reuse=None,
             repeat=1):
    """(compile result, run outcome or None, engines) for one pass spec.
    `reuse` is an earlier result, returned when the MIR is the same."""
    mir = work / f"{tag}.mir"
    comp = compile_model(ctx, source, passes, mir, repeat)
    if reuse and comp["sha"] and comp["sha"] == reuse[0]["sha"]:
        return reuse
    if comp["outcome"] != "ok":
        return comp, None, None
    run, engines = check_model(ctx, source, data_file, mir,
                               work / f"{tag}.status.jsonl")
    return comp, run, engines


def differences(base, other):
    """Differences of `other` from `base`; each is evaluate()'s triple."""
    diffs, reason = compare_compiles(base[0], other[0])
    if diffs is not None:
        return diffs, reason
    return compare_runs(base[1], other[1])


def bisect_steps():
    """(added pass, pass list) for each bisect step.

    function_inlining leaves nested statement lists that list_collapsing
    flattens, and stanli does not write transformed parameters whose
    declarations sit in such a list. O1 always flattens them, so every step
    runs list_collapsing too, and the first step tries it alone."""
    steps = [("list_collapsing", "list_collapsing")]
    for i, added in enumerate(PRODUCTION_PASSES):
        if added == "list_collapsing":
            continue
        chosen = PRODUCTION_PASSES[:i + 1] + ("list_collapsing",)
        steps.append((added, ",".join(p for p in PRODUCTION_PASSES
                                      if p in chosen)))
    return steps


def guilty_step(steps, target):
    """The pass added at the first step whose primary difference kind is
    o1's. An intermediate pass list can make MIR that stanli refuses or
    reads differently although o1 does not, so a step that differs in
    another way is passed over. Without a match, the first step that
    differs at all, marked with "?"."""
    for s in steps:
        if s["primary"] == target:
            return s["added"]
    for s in steps:
        if s["primary"] is not None:
            return s["added"] + "?"
    return "unreproduced"


def bisect(ctx, source, data_file, work, none, o1):
    """(guilty pass, steps) for a model whose o1 answer differs."""
    target = primary_kind(differences(none, o1)[0])
    steps, previous = [], none
    for i, (added, spec) in enumerate(bisect_steps()):
        result = evaluate(ctx, source, data_file, work, spec, f"step{i}",
                          reuse=previous)
        diffs, reason = differences(none, result)
        steps.append({"added": added, "size": result[0]["size"],
                      "kinds": sorted({k for k, _ in diffs}),
                      "primary": primary_kind(diffs) if diffs else None,
                      "not_comparable": reason})
        previous = result
        if diffs and steps[-1]["primary"] == target:
            break
    guilty = guilty_step(steps, target)
    if guilty == "unreproduced":
        return guilty, steps
    # Guard: the full cumulative list must reproduce the o1 MIR.
    full = compile_model(ctx, source, ",".join(PRODUCTION_PASSES),
                         work / "full.mir")
    if full.get("sha") != o1[0].get("sha"):
        return f"{guilty}?bisect_invalid", steps
    return guilty, steps


def one_model(name, source, data_obj, ctx):
    source = pathlib.Path(source).resolve()
    record = {"model": name, "source": str(source)}
    if data_obj is None:
        record["not_comparable"] = "no_recorded_data"
        return record
    work = pathlib.Path(tempfile.mkdtemp(prefix="fuzz_", dir=ctx["work"]))
    try:
        data_file = work / "data.json"
        if isinstance(data_obj, pathlib.Path):
            shutil.copyfile(data_obj, data_file)
        else:
            data_file.write_text(json.dumps(data_obj))
        none = evaluate(ctx, source, data_file, work, "none", "none",
                        repeat=2)
        o1 = evaluate(ctx, source, data_file, work, "o1", "o1", repeat=2)
        for tag, r in (("none", none), ("o1", o1)):
            record[tag] = {"compile": {k: r[0][k] for k in
                                       ("outcome", "seconds", "size",
                                        "message")},
                           "run": None if r[1] is None else r[1][0],
                           "run_message": (r[1][1][:300] if r[1] and
                                           isinstance(r[1][1], str)
                                           else None),
                           "engines": r[2]}
        diffs, reason = differences(none, o1)
        if reason:
            record["not_comparable"] = reason
            return record
        record["engines_match"] = none[2] == o1[2]
        record["mir_identical"] = none[0]["sha"] == o1[0]["sha"]
        record["points_with_values"] = values_compared(none[1], o1[1])
        if not diffs:
            record["agree"] = True
            return record
        record["diffs"] = [{"kind": k, **d} for k, d in diffs]
        guilty, steps = bisect(ctx, source, data_file, work, none, o1)
        record["guilty"] = guilty
        record["bisect"] = steps
        record["bucket"] = bucket(guilty, diffs)
        keep = ctx["keep"] / name.replace("/", "__")
        keep.mkdir(parents=True, exist_ok=True)
        for f in ("none.mir", "o1.mir", "data.json"):
            if (work / f).exists():
                shutil.copyfile(work / f, keep / f)
        return record
    except Exception as e:  # report the model, keep the sweep going
        record["not_comparable"] = f"harness_error: {e!r}"
        return record
    finally:
        shutil.rmtree(work, ignore_errors=True)


# ---------------------------------------------------------------------------
# Inputs and summary.


def stanc3_good_inputs():
    good.check_corpus()
    data = good.load_gz(good.DATA)
    overrides = json.loads(good.OVERRIDES.read_text())
    for path in good.corpus_models():
        source, obj = good.model_input(path, overrides, data)
        yield path, source, obj


def brms_inputs():
    for source in sorted((REPO / "tests" / "brms").glob("*.stan")):
        yield f"brms/{source.name}", source, source.with_suffix(".json")


def retime(ctx, results, count, repeat=5):
    """Time the compiles with the largest o1/none ratios again, one at a
    time, keeping each side's fastest of `repeat` runs. The parallel sweep
    shares the machine with the other jobs and is too noisy to rank.
    A negative count times every model."""
    rows = []
    for r in results:
        a = (r.get("none") or {}).get("compile", {}).get("seconds")
        b = (r.get("o1") or {}).get("compile", {}).get("seconds")
        if a and b:
            rows.append((b / a, r))
    rows.sort(key=lambda row: -row[0])
    if count >= 0:
        rows = rows[:count]
    with tempfile.TemporaryDirectory(dir=ctx["work"]) as work:
        for _, r in rows:
            source = pathlib.Path(r["source"])
            r["serial_seconds"] = {
                passes: compile_model(ctx, source, passes,
                                      pathlib.Path(work) / "x.mir",
                                      repeat)["seconds"]
                for passes in ("none", "o1")}


def summarize(results, out):
    def p(line=""):
        print(line, file=out)

    buckets, nc = {}, {}
    agree = 0
    for r in results:
        if "bucket" in r:
            buckets.setdefault(r["bucket"], []).append(r)
        elif "not_comparable" in r:
            reason = r["not_comparable"].split(":")[0]
            nc[reason] = nc.get(reason, 0) + 1
        elif r.get("agree"):
            agree += 1
    p(f"{len(results)} models: {agree} agree, "
      f"{sum(map(len, buckets.values()))} differ, "
      f"{sum(nc.values())} not comparable")
    agreeing = [r for r in results if r.get("agree")]
    p(f"of those that agree: {sum(r['mir_identical'] for r in agreeing)} "
      f"had identical MIR, "
      f"{sum(r['points_with_values'] == 0 for r in agreeing)} compared only "
      f"errors, {sum(not r['engines_match'] for r in agreeing)} ran on "
      f"different engines")
    p()
    p("bucket (first guilty pass:kind)          models  engines differ")
    for b, rs in sorted(buckets.items(), key=lambda kv: -len(kv[1])):
        mixed = sum(1 for r in rs if not r.get("engines_match", True))
        p(f"  {b:40s} {len(rs):5d}  {mixed:5d}")
    p()
    p("not comparable")
    for reason, n in sorted(nc.items(), key=lambda kv: -kv[1]):
        p(f"  {reason:40s} {n:5d}")
    for b, rs in sorted(buckets.items(), key=lambda kv: -len(kv[1])):
        p()
        p(f"examples for {b}")
        for r in sorted(rs, key=lambda r: r["o1"]["compile"]["size"] or 0)[:3]:
            p(f"  {r['model']}")
            for d in r["diffs"][:3]:
                p(f"    {json.dumps(d)[:300]}")

    def ratios(key):
        rows = []
        for r in results:
            a = (r.get("none") or {}).get("compile", {}).get(key)
            b = (r.get("o1") or {}).get("compile", {}).get(key)
            if a and b:
                rows.append((b / a, a, b, r["model"]))
        return sorted(rows, reverse=True)[:10]

    timed = [(r["serial_seconds"]["o1"] / r["serial_seconds"]["none"], r)
             for r in results if r.get("serial_seconds")]
    if timed:
        p()
        p(f"largest o1/none compile time ratios, timed serially, fastest of "
          f"5, over {len(timed)} models")
        for ratio, r in sorted(timed, key=lambda t: -t[0])[:10]:
            t = r["serial_seconds"]
            p(f"  {ratio:7.2f}  none {t['none']:.3f} s, o1 {t['o1']:.3f} s  "
              f"{r['model']}")
    for key, unit in (("seconds", "s (parallel, noisy)"),
                      ("size", "bytes")):
        p()
        p(f"largest o1/none compile {key} ratios")
        for ratio, a, b, model in ratios(key):
            fmt = (lambda v: f"{v:.3f}") if key == "seconds" else str
            p(f"  {ratio:7.2f}  none {fmt(a)} {unit}, o1 {fmt(b)} {unit}  "
              f"{model}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pairs", nargs="*", help="MODEL.stan DATA.json ...")
    ap.add_argument("--corpus", choices=("stanc3_good", "brms"))
    ap.add_argument("--compiler", type=pathlib.Path,
                    default=REPO / "deps" / "stanc3" / "stanli-compile")
    ap.add_argument("--check", type=pathlib.Path,
                    default=REPO / "build-rel" / "stanli_check")
    ap.add_argument("--jobs", type=int, default=24)
    ap.add_argument("--timeout", type=float, default=TIMEOUT)
    ap.add_argument("--out", type=pathlib.Path,
                    default=pathlib.Path("fuzz_stanc_results.json"))
    ap.add_argument("--match", help="only models whose name contains this")
    ap.add_argument("--retime", type=int, default=10, metavar="N",
                    help="re-time the N largest compile-time ratios "
                    "serially; -1 re-times every model")
    args = ap.parse_args(argv)
    if len(args.pairs) % 2:
        ap.error("expected MODEL DATA pairs")
    inputs = []
    if args.corpus == "stanc3_good":
        inputs = list(stanc3_good_inputs())
    elif args.corpus == "brms":
        inputs = list(brms_inputs())
    for model, data in zip(args.pairs[::2], args.pairs[1::2]):
        inputs.append((model, pathlib.Path(model),
                       json.loads(pathlib.Path(data).read_text())))
    if args.match:
        inputs = [i for i in inputs if args.match in i[0]]
    for tool in (args.compiler, args.check):
        if not tool.exists():
            ap.error(f"{tool} does not exist")
    out_dir = args.out.resolve().parent
    out_dir.mkdir(parents=True, exist_ok=True)
    work = pathlib.Path(tempfile.mkdtemp(prefix="fuzz_stanc_"))
    ctx = {"compiler": args.compiler.resolve(), "check": args.check.resolve(),
           "timeout": args.timeout, "work": work,
           "keep": out_dir / (args.out.stem + "_mismatches")}
    started = time.monotonic()
    results = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = [pool.submit(one_model, name, src, data, ctx)
                   for name, src, data in inputs]
        for n, f in enumerate(concurrent.futures.as_completed(futures), 1):
            results.append(f.result())
            if n % 100 == 0:
                print(f"{n}/{len(futures)} models", file=sys.stderr)
    retime(ctx, results, args.retime)
    shutil.rmtree(work, ignore_errors=True)
    results.sort(key=lambda r: r["model"])
    args.out.write_text(json.dumps(
        {"compiler": str(args.compiler), "check": str(args.check),
         "points": POINTS, "rel_tol": REL_TOL, "abs_tol": ABS_TOL,
         "timeout": args.timeout, "seconds": time.monotonic() - started,
         "models": results}, indent=1) + "\n")
    with open(args.out.with_suffix(".txt"), "w") as f:
        summarize(results, f)
    summarize(results, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
