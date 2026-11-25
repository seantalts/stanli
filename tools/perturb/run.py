#!/usr/bin/env python3
"""Find performance cliffs in rewritten corpus models.

    tools/perturb/run.py VARIANTS --out OUT [--stage validity|structure|timing|all|scaling]

VARIANTS/<seed>/ holds base.stan and base.json (the corpus model and data)
and, per variant, <variant>.stan, an optional <variant>.json and a
<variant>.meta.json. The stages are described in README.md; each --stage
runs the ones before it too, except scaling, which sweeps data sizes.
OUT/results.json keeps every measurement and OUT/report.md ranks them.

    tools/perturb/run.py init VARIANTS MODEL[,MODEL...]

copies corpus models in as seeds.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import math
import pathlib
import re
import shutil
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import measure as m  # noqa: E402
import differential  # noqa: E402  (on the path through measure)

FLAG_RATIO = 1.3
SCREEN_ROUNDS = 6
CONFIRM_ROUNDS = 24
LOAD_WARNING = 6.0
STAGES = ("validity", "structure", "timing", "all")
# Differences that make an equivalent variant invalid. write_array, printed
# output and error wording may change with a rewrite.
INVALID_KINDS = ("lp", "grad", "one_side_threw", "crash", "timeout")
# An equivalent variant whose lp and gradient differ from the base's by less
# than this (relative) is near_equivalent: it goes on to the performance
# stages, since the last digits do not change the performance question.
NEAR_TOL = 1e-7
# An equivalent variant whose preparation takes this much longer than the
# base's, by both measures, is a prep_cliff.
PREP_CLIFF_RATIO = 3.0
PREP_CLIFF_S = 0.1
PROPTO = re.compile(r"~|_lupdf\b|_lupmf\b")


ratio_text = m.ratio_text


# ---------------------------------------------------------------------------
# Inputs


def load_variants(root, only=None):
    """[(seed, name, stan, data, meta)] for every variant under root."""
    out = []
    for seed_dir in sorted(p for p in pathlib.Path(root).iterdir()
                           if (p / "base.stan").exists()):
        for stan in sorted(seed_dir.glob("*.stan")):
            name = stan.stem
            if name == "base":
                continue
            key = f"{seed_dir.name}/{name}"
            if only and not any(o in key for o in only):
                continue
            data = seed_dir / f"{name}.json"
            meta_path = seed_dir / f"{name}.meta.json"
            meta = (json.loads(meta_path.read_text()) if meta_path.exists()
                    else None)
            out.append((seed_dir.name, name, stan,
                        data if data.exists() else seed_dir / "base.json", meta))
    return out


def init_seeds(root, models):
    sys.path.insert(0, str(m.REPO / "tools"))
    from corpus_inventory import corpus_cases, materialize_data
    cases = corpus_cases(m.REPO / "deps" / "posteriordb", "all")
    for name in models:
        case = cases[name]
        dest = pathlib.Path(root) / name
        dest.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(case.source, dest / "base.stan")
        with tempfile.TemporaryDirectory() as tmp:
            shutil.copyfile(materialize_data(case, tmp), dest / "base.json")
        print(f"{dest}: base.stan, base.json")


# ---------------------------------------------------------------------------
# Stage 1: validity


def first_problem(diffs):
    for kind, d in diffs:
        if kind in INVALID_KINDS and not d.get("write_array"):
            text = json.dumps(d).replace('"none"', '"base"').replace(
                '"o1"', '"variant"')
            return f"{kind} differs: {text[:300]}"
    return None


def validity(tools, rec, stan, data, base_stan, base_data):
    meta = rec["meta"]
    if meta is None:
        return "no meta.json"
    base_c = m.compile_source(tools, base_stan.read_text())
    if not base_c["ok"]:
        return f"base does not compile: {base_c['error']}"
    var_c = m.compile_source(tools, stan.read_text())
    rec["compile_s"] = var_c["seconds"]
    rec["base_compile_s"] = base_c["seconds"]
    if not var_c["ok"]:
        return f"does not compile: {var_c['error']}"
    base_out, _ = m.check(tools, base_c, base_data)
    why = m.finite_values(base_out)
    if why:
        return f"base fails at the check points: {why}"
    var_out, _ = m.check(tools, var_c, data)
    why = m.finite_values(var_out)
    if why:
        return f"variant fails: {why}"
    if meta.get("equivalent"):
        propto = bool(PROPTO.search(base_stan.read_text())
                      or PROPTO.search(stan.read_text()))
        diffs, _ = differential.compare_runs(base_out, var_out, propto)
        problem = first_problem(diffs)
        if problem:
            kinds = {k for k, d in diffs if k in INVALID_KINDS
                     and not d.get("write_array")}
            worst = largest_difference(base_out, var_out, propto)
            if kinds <= {"lp", "grad"} and worst is not None and worst < NEAR_TOL:
                rec["near_equivalent"] = {"largest_relative_difference": worst,
                                          "first": problem}
            else:
                return problem
        if propto and differential.compare_runs(base_out, var_out)[0]:
            rec["lp_constant_shift"] = True
    return None


def largest_difference(a, b, propto):
    """The largest relative difference of lp and gradient entries over the
    check points, or None when the values are not comparable. Under propto
    the lp may move by a constant, so only the gradient counts."""
    worst = 0.0
    for k in differential.POINTS:
        sa = a[1]["points"][k]["status"]
        sb = b[1]["points"][k]["status"]
        if not sa or not sb or sa[0] != "OK" or sb[0] != "OK" or \
                len(sa[1]) != len(sb[1]):
            return None
        pairs = list(zip(sa[1], sb[1]))[1 if propto else 0:]
        for x, y in pairs:
            x, y = float(x), float(y)
            scale_ = max(abs(x), abs(y))
            if scale_ > differential.ABS_TOL:
                worst = max(worst, abs(x - y) / scale_)
    return worst


# ---------------------------------------------------------------------------
# Stage 2: structure


def structure(tools, rec, stan, data, base_stan, base_data):
    base = m.structure(tools, m.compile_source(tools, base_stan.read_text()),
                       base_data)
    var = m.structure(tools, m.compile_source(tools, stan.read_text()), data)
    rec["base_program"], rec["variant_program"] = base, var
    if base.get("failure"):
        return "error", f"base: {base['failure']}"
    if var.get("failure"):
        return "failure", var["failure"]
    rec["new_declines"] = sorted(set(var["declines"]) - set(base["declines"]))
    rec["differs"] = [part for part, key in (
        ("log_prob program", "ops_text_sha"),
        ("write_array program", "wa_text_sha"), ("islands", "islands_sha"))
        if base.get(key) != var.get(key)]
    if rec["meta"].get("equivalent") and not rec["differs"]:
        return "same_program", None
    if prep_cliff(rec):
        rec["prep_cliff"] = True
    return None, None


def prep_cliff(rec):
    """Does an equivalent variant take much longer to prepare than its base?"""
    a = (rec.get("base_program") or {}).get("prep_s")
    b = (rec.get("variant_program") or {}).get("prep_s")
    return bool(rec["meta"].get("equivalent") and a is not None and b is not None
                and b >= PREP_CLIFF_RATIO * a and b - a >= PREP_CLIFF_S)


def cached_structure(tools, cache, stan, data):
    key = (str(stan), str(data))
    if key not in cache:
        cache[key] = m.structure(
            tools, m.compile_source(tools, pathlib.Path(stan).read_text()),
            data)
    return cache[key]


# ---------------------------------------------------------------------------
# Stages 3 and 4: timing


def expected_work(meta):
    """The work multiple a non-equivalent variant should cost, from its
    meta's "work"; None when it gives none. "dose" is only a label."""
    work = meta.get("work")
    return None if work is None else float(work)


def needs_cmdstan(rec):
    meta, t = rec["meta"], rec["timing"]
    if meta.get("equivalent"):
        return t["ratio"] >= FLAG_RATIO
    work = expected_work(meta)
    if work is not None:
        return t["ratio"] >= FLAG_RATIO * work
    return True


def timing_cmds(tools, rec, paths):
    """The bench_grad commands for base and variant, and their points."""
    stan, data, base_stan, base_data = paths
    base_c = m.compile_source(tools, base_stan.read_text())
    var_c = m.compile_source(tools, stan.read_text())
    points, source = None, "fixed points (the variant is not equivalent)"
    if rec["meta"].get("equivalent"):
        points, source = m.committed_points(tools, rec["seed"], base_stan,
                                            base_data)
    rec["points"] = source
    return (m.stanli_cmd(tools, base_c, base_data, points),
            m.stanli_cmd(tools, var_c, data, points), points)


def time_variant(tools, rec, paths):
    a, b, points = timing_cmds(tools, rec, paths)
    rec["timing"] = m.paired_ratio(a, b, SCREEN_ROUNDS, tools.timeout)
    big = max((rec.get(f"{side}_program") or {}).get("executor_bytes") or 0
              for side in ("base", "variant"))
    if big > m.BIG_EXECUTOR:
        rec["copies"] = m.copies_record(a, b, m.COPIES, tools.timeout)
    return a, b, points


def normalize(tools, rec, paths, cmds, args):
    stan, data, base_stan, base_data = paths
    a, b, points = cmds
    drivers = [m.cmdstan_driver(tools, p.read_text()) for p in (base_stan, stan)]
    for d in drivers:
        if "error" in d:
            rec["cmdstan_error"] = d["error"]
            return "error"
    rec["confirm"] = m.paired_ratio(a, b, CONFIRM_ROUNDS, tools.timeout)
    rec["cmdstan"] = m.paired_ratio(m.cmdstan_cmd(drivers[0], base_data, points),
                                    m.cmdstan_cmd(drivers[1], data, points),
                                    SCREEN_ROUNDS, tools.timeout)
    rec["cliff"] = rec["confirm"]["ratio"] / rec["cmdstan"]["ratio"]
    rec["stanli_vs_cmdstan"] = rec["confirm"]["b_ns"] / rec["cmdstan"]["b_ns"]
    if args.copies and "copies" not in rec:
        rec["copies"] = m.copies_record(a, b, args.copies, tools.timeout)
    if rec["cliff"] >= FLAG_RATIO:
        return "cliff"
    work = expected_work(rec["meta"])
    if rec["confirm"]["ratio"] < FLAG_RATIO * (1.0 if work is None else work):
        return "fine"
    return "flagged_cmdstan_equal"


# Fields that stages 3 and 4 add to a record, kept per variant in
# OUT/cache/timing so that an interrupted run resumes where it stopped.
TIMED_FIELDS = ("outcome", "reason", "points", "timing", "timing_s", "copies",
                "confirm", "cmdstan", "cliff", "stanli_vs_cmdstan",
                "cmdstan_error", "cmdstan_s")


def timing_key(tools, paths, meta):
    """A hash of everything a variant's timing depends on."""
    stan, data, base_stan, base_data = paths
    parts = [m.sha_file(f) for f in (stan, data, base_stan, base_data,
                                     tools.bench)]
    parts.append(json.dumps(meta, sort_keys=True))
    return m.sha_text("\n".join(parts))


def load_timed(tools, key):
    path = tools.cache / "timing" / f"{key[:24]}.json"
    return json.loads(path.read_text()) if path.exists() else None


def save_timed(tools, key, rec, phase):
    path = tools.cache / "timing" / f"{key[:24]}.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"phase": phase, **{
        k: rec[k] for k in TIMED_FIELDS if k in rec}}))


# ---------------------------------------------------------------------------
# Driver


def validity_line(rec):
    key = f"{rec['seed']}/{rec['variant']}"
    if rec["outcome"] == "invalid":
        return f"INVALID {key}: {rec['reason']}"
    kind = "equivalent" if rec["meta"].get("equivalent") else "not equivalent"
    if rec.get("near_equivalent"):
        kind = (f"near_equivalent, largest relative difference "
                f"{rec['near_equivalent']['largest_relative_difference']:.1e}")
    shift = ", lp shifted by a constant" if rec.get("lp_constant_shift") else ""
    if "variant_program" not in rec:
        return f"valid   {key} ({kind}{shift})"
    b, v = rec["base_program"], rec["variant_program"]
    prep = (f"preparation {b['prep_s'] * 1e3:.0f} -> {v['prep_s'] * 1e3:.0f} ms"
            if b.get("prep_s") is not None and v.get("prep_s") is not None else "")
    cliff = ", PREP_CLIFF" if rec.get("prep_cliff") else ""
    return (f"changed {key} ({kind}{shift}): {', '.join(rec['differs'])} "
            f"differ; ops {b.get('ops')} -> {v.get('ops')} (before the passes "
            f"{b.get('lowered_ops')} -> {v.get('lowered_ops')}); {prep}{cliff}")


def run_variants(args, tools):
    variants = load_variants(args.variants, args.only)
    if not variants:
        raise SystemExit(f"no variants under {args.variants}")
    records, paths = [], {}
    for seed, name, stan, data, meta in variants:
        rec = {"seed": seed, "variant": name, "meta": meta,
               "class": (meta or {}).get("class"),
               "dose": (meta or {}).get("dose"), "data": data.name}
        records.append(rec)
        paths[id(rec)] = (stan, data, stan.parent / "base.stan",
                          stan.parent / "base.json")
    stage = STAGES.index(args.stage)

    def stage12(rec):
        p = paths[id(rec)]
        started = time.monotonic()
        try:
            reason = validity(tools, rec, *p)
            rec["validity_s"] = time.monotonic() - started
            if reason:
                rec["outcome"], rec["reason"] = "invalid", reason
                return rec
            rec["outcome"] = "valid"
            if stage >= 1:
                started = time.monotonic()
                outcome, reason = structure(tools, rec, *p)
                rec["structure_s"] = time.monotonic() - started
                if outcome:
                    rec["outcome"], rec["reason"] = outcome, reason
        except Exception as e:  # keep the run going
            rec["outcome"], rec["reason"] = "error", f"harness: {e!r}"
        return rec

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for rec in pool.map(stage12, records):
            print(validity_line(rec) if rec["outcome"] in ("valid", "invalid")
                  else f"{rec['outcome']:7s} {rec['seed']}/{rec['variant']}: "
                       f"{rec.get('reason') or ''}", flush=True)
    meta = {"stage": args.stage, "load_1min": [m.load_average()]}
    if stage >= 2:
        timed = [r for r in records if r["outcome"] == "valid"]
        flagged = []
        resumed = 0
        for rec in timed:
            p = paths[id(rec)]
            key = rec["timing_key"] = timing_key(tools, p, rec["meta"])
            cached = load_timed(tools, key)
            if cached and (cached["phase"] == "done" or stage < 3):
                rec.update({k: v for k, v in cached.items() if k != "phase"})
                resumed += 1
                continue
            meta["load_1min"].append(m.load_average())
            started = time.monotonic()
            try:
                if cached:
                    rec.update({k: v for k, v in cached.items() if k != "phase"})
                    cmds = timing_cmds(tools, rec, p)
                else:
                    cmds = time_variant(tools, rec, p)
                    rec["timing_s"] = time.monotonic() - started
                if needs_cmdstan(rec) and stage >= 3:
                    flagged.append((rec, p, cmds))
                    save_timed(tools, key, rec, "timed")
                else:
                    rec["outcome"] = "flagged" if needs_cmdstan(rec) else "fine"
                    save_timed(tools, key, rec, "done")
            except Exception as e:
                rec["outcome"], rec["reason"] = "error", f"timing: {e!r}"
            print(f"timed   {rec['seed']}/{rec['variant']}: "
                  f"{ratio_text(rec.get('timing', {}).get('ratio'))}", flush=True)
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            list(pool.map(lambda f: [m.cmdstan_driver(tools, p.read_text())
                                     for p in (f[1][2], f[1][0])], flagged))
        for rec, p, cmds in flagged:
            meta["load_1min"].append(m.load_average())
            started = time.monotonic()
            try:
                rec["outcome"] = normalize(tools, rec, p, cmds, args)
                rec["cmdstan_s"] = time.monotonic() - started
                save_timed(tools, rec["timing_key"], rec, "done")
            except Exception as e:
                rec["outcome"], rec["reason"] = "error", f"cmdstan: {e!r}"
            print(f"{rec['outcome']:7s} {rec['seed']}/{rec['variant']}: "
                  f"{m.normalized_text(rec.get('cliff'))}", flush=True)
        meta["resumed"] = resumed
    return records, meta


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    if argv[:1] == ["init"]:
        if len(argv) != 3:
            raise SystemExit("usage: run.py init VARIANTS MODEL[,MODEL...]")
        init_seeds(argv[1], argv[2].split(","))
        return 0
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("variants", type=pathlib.Path)
    ap.add_argument("--out", type=pathlib.Path, required=True)
    ap.add_argument("--stage", choices=STAGES + ("scaling", "datavalues"),
                    default="all")
    ap.add_argument("--jobs", type=int, default=28)
    ap.add_argument("--only", action="append",
                    help="only variants whose SEED/VARIANT contains this")
    ap.add_argument("--copies", type=int, default=0,
                    help="also time N concurrent copies of each flagged "
                    "variant and its base")
    ap.add_argument("--build", type=pathlib.Path, default=m.REPO / "build-rel")
    ap.add_argument("--known", type=pathlib.Path, default=HERE / "known.json")
    ap.add_argument("--timeout", type=float, default=300.0)
    ap.add_argument("--max-seconds", type=float, default=60.0,
                    help="scaling: stop a ladder when preparation or one "
                    "check is predicted to take longer than this")
    ap.add_argument("--max-rss-gb", type=float, default=6.0,
                    help="scaling: stop a ladder before peak RSS passes this")
    ap.add_argument("--no-timing", action="store_true",
                    help="scaling: structural ladder only; scaled data "
                    "files are deleted after they are measured")
    ap.add_argument("--growth", choices=("resample", "tile"),
                    default="resample",
                    help="scaling: how a ladder grows an axis")
    ap.add_argument("--model-seconds", type=float, default=180.0,
                    help="scaling: time budget for one model's ladders")
    ap.add_argument("--memory-gb", type=float, default=6.0,
                    help="kill any tool process whose RSS passes this")
    args = ap.parse_args(argv)
    args.out.mkdir(parents=True, exist_ok=True)
    tools = m.Tools(args.build, args.out, args.timeout)
    m.MEMORY_LIMIT = int(args.memory_gb * 2**30) if args.memory_gb else None
    known = json.loads(args.known.read_text())["known"]
    started = time.monotonic()
    import report
    if args.stage == "scaling":
        import ladder
        results, meta = ladder.run_scaling(args, tools)
        meta["seconds"] = time.monotonic() - started
        (args.out / "scaling.json").write_text(json.dumps(
            {**meta, "models": results}, indent=1) + "\n")
        text = report.scaling_report(results, meta, known)
        (args.out / "scaling.md").write_text(text)
    elif args.stage == "datavalues":
        import datavary
        results, meta = datavary.run_datavalues(args, tools)
        meta["seconds"] = time.monotonic() - started
        (args.out / "datavalues.json").write_text(json.dumps(
            {**meta, "models": results}, indent=1) + "\n")
        (args.out / "datavalues.md").write_text(datavary.report(results, meta))
    else:
        records, meta = run_variants(args, tools)
        meta["seconds"] = time.monotonic() - started
        (args.out / "results.json").write_text(json.dumps(
            {**meta, "variants": records}, indent=1) + "\n")
        text = report.variant_report(records, meta, known)
        (args.out / "report.md").write_text(text)
    print(f"\nwrote {args.out}; {meta['seconds']:.0f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
