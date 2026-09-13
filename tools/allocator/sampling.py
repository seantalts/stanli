#!/usr/bin/env python3
"""Matched shipping Python source-to-fit measurements, without artificial warmup."""
import argparse
import json
import os
import pathlib
import platform
import random
import resource
import statistics
import subprocess
import sys
import time

from bench import ROOT, save, sha

MODELS = ["eight_schools_noncentered", "radon_pooled", "hierarchical_gp",
          "normal_1024", "normal_262144", "gamma_128", "lotka_volterra"]
SLOTS = ["system", "candidate", "system_aa", "candidate_aa"]
SEED = 49201


def worker(args):
    started = time.perf_counter()
    import numpy as np
    import stanli
    imported = time.perf_counter()
    assert ("mimalloc" in stanli.build_id()) == (args.mode == "private")
    source = args.source.read_text()
    data = json.loads(args.data.read_text())
    model = stanli.Model(stan_code=source, data=data, seed=SEED)
    prepared = time.perf_counter()
    fit = model.sample(chains=args.workers, parallel_chains=args.workers,
                       seed=SEED, warmup=args.warmup, samples=args.samples,
                       inits=np.zeros(model.n_unconstrained), refresh=0)
    finished = time.perf_counter()
    arrays = [fit.draws(), fit.sampler_stats,
              fit._divergence_counts, fit._max_treedepth_counts]
    assert all(a is not None for a in arrays)
    assert np.isfinite(fit.draws()).all()
    header = dict(names=fit.names, shapes=[a.shape for a in arrays],
                  dtypes=[a.dtype.str for a in arrays])
    with args.snapshot.open("wb") as output:
        output.write(json.dumps(header, sort_keys=True).encode() + b"\n")
        for array in arrays:
            output.write(np.ascontiguousarray(array).tobytes())
    peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    print(json.dumps(dict(kind="sampling", build_id=stanli.build_id(),
        import_ns=(imported - started) * 1e9, prepare_ns=(prepared - imported) * 1e9,
        sample_ns=(finished - prepared) * 1e9, total_ns=(finished - imported) * 1e9,
        peak_rss_bytes=peak * (1 if sys.platform == "darwin" else 1024),
        header=header, divergences=fit._divergence_counts.tolist(),
        max_depth=fit._max_treedepth_counts.tolist(), numpy=np.__version__,
        snapshot_sha256=sha(args.snapshot))), flush=True)


def environment(package):
    result = {k: v for k, v in os.environ.items() if not k.startswith(
        ("DYLD_", "LD_PRELOAD", "LD_AUDIT", "MIMALLOC_", "TCMALLOC_", "Malloc", "STANLI_"))}
    result["PYTHONPATH"] = str(package.resolve())
    result["PYTHONDONTWRITEBYTECODE"] = "1"
    return result


def manifest(args):
    cases = {c["name"]: c for c in json.loads(args.inputs.read_text())["cases"]}
    names = ["eight_schools_noncentered"] if args.smoke else MODELS
    for name in names:
        case = cases[name]
        for file, digest in case["sha256"].items():
            assert sha(pathlib.Path(case["input_dir"]) / file) == digest
    packages = {}
    for slot in ("system", "candidate"):
        package = getattr(args, slot).resolve()
        packages[slot] = dict(path=str(package), sha256={
            str(p.relative_to(package)): sha(p) for p in sorted(package.rglob("*"))
            if p.is_file() and "__pycache__" not in p.parts})
    return dict(inputs_sha256=sha(args.inputs), cases={n: cases[n] for n in names},
        source_sha256=sha(__file__), packages=packages, seed=SEED,
        warmup=20 if args.smoke else 500, samples=20 if args.smoke else 500,
        cells=[[name, w] for name in names for w in (1, 4)], slots=SLOTS,
        platform=platform.platform(), python=sys.executable,
        head=subprocess.check_output(["git", "-c", "safe.directory=" + str(ROOT),
            "rev-parse", "HEAD"], cwd=ROOT, text=True).strip())


def execute(args):
    out = args.output.resolve()
    phase = out / args.phase
    phase.mkdir(parents=True)
    (phase / "logs").mkdir()
    fixed = manifest(args)
    save(phase / "manifest.json", fixed)
    if args.phase == "verify":
        calibration = {}
    else:
        frozen = json.loads((out / "verification.json").read_text())
        assert fixed == frozen["manifest"], "Sampling inputs/packages/harness changed"
        calibration = frozen["snapshots"]
    records = []

    def one(cell, slot, rnd):
        name, workers = cell
        alias = slot.removesuffix("_aa")
        case = fixed["cases"][name]
        base = phase / "logs" / f"{len(records):04d}-{name}-w{workers}-{slot}"
        command = [sys.executable, __file__, "worker", "--mode",
            "system" if alias == "system" else "private", "--source",
            str(pathlib.Path(case["input_dir"]) / "model.stan"), "--data",
            str(pathlib.Path(case["input_dir"]) / "data.json"), "--workers", str(workers),
            "--warmup", str(fixed["warmup"]), "--samples", str(fixed["samples"]),
            "--snapshot", str(base.with_suffix(".snapshot"))]
        begin = time.time()
        result = subprocess.run(command, env=environment(pathlib.Path(fixed["packages"][alias]["path"])),
                                text=True, capture_output=True, timeout=900)
        base.with_suffix(".stdout").write_text(result.stdout)
        base.with_suffix(".stderr").write_text(result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        record = dict(name=name, workers=workers, slot=slot, round=rnd, command=command,
                      returncode=result.returncode, process_seconds=time.time() - begin,
                      result=rows[-1] if rows else {})
        records.append(record)
        with (phase / "runs.jsonl").open("a") as index:
            index.write(json.dumps(record) + "\n")
        assert result.returncode == 0 and len(rows) == 1, record
        row = record["result"]
        assert row["snapshot_sha256"] == sha(base.with_suffix(".snapshot"))
        key = f"{name}/{workers}"
        if key not in calibration:
            calibration[key] = row["snapshot_sha256"]
        assert row["snapshot_sha256"] == calibration[key], "Full sampling results differ"

    if args.phase == "verify":
        for cell in fixed["cells"]:
            for slot in ("system", "candidate"):
                one(cell, slot, -1)
            print("sampling verified", cell, flush=True)
        save(out / "verification.json", dict(manifest=fixed, snapshots=calibration))
    else:
        cells = fixed["cells"]
        rounds = 1 if args.smoke else 5
        if args.phase == "confirm":
            screen = json.loads((out / "bench/summary.json").read_text())
            selected = {(c["name"], c["workers"]) for c in screen["cells"]
                        if any(c["ratios"][metric]["candidate"]["median"] < 0.97
                               for metric in ("total_ns", "sample_ns"))}
            if selected:
                selected |= {("eight_schools_noncentered", 1), ("normal_1024", 4)} & {
                    tuple(cell) for cell in fixed["cells"]}
            cells = sorted(selected)
            rounds = 7
        schedule = []
        for rnd in range(rounds):
            shuffled = list(cells)
            random.Random(84013 + rnd).shuffle(shuffled)
            slots = SLOTS[rnd % 4:] + SLOTS[:rnd % 4]
            if rnd % 2:
                slots = slots[::-1]
            for cell in shuffled:
                for slot in slots:
                    schedule.append(dict(cell=cell, slot=slot, round=rnd))
        save(phase / "schedule.json", schedule)
        for step in schedule:
            one(step["cell"], step["slot"], step["round"])
            if len(records) % 4 == 0:
                print("sampling", args.phase, step["round"] + 1, step["cell"], flush=True)
        save(phase / "summary.json", summarize(records, cells, rounds))
    save(phase / "completion.json", dict(processes=len(records), finished=time.time()))


def summarize(records, cells, rounds):
    result = []
    for name, workers in cells:
        selected = [r for r in records if (r["name"], r["workers"]) == (name, workers)]
        assert len(selected) == 4 * rounds
        assert len({r["result"]["snapshot_sha256"] for r in selected}) == 1
        ratios, absolute = {}, {}
        for metric in ("total_ns", "sample_ns", "prepare_ns", "import_ns", "peak_rss_bytes"):
            groups = {slot: [next(r["result"][metric] for r in selected
                if r["slot"] == slot and r["round"] == rnd) for rnd in range(rounds)] for slot in SLOTS}
            absolute[metric] = {s: statistics.median(v) for s, v in groups.items()}
            ratios[metric] = {}
            for label, a, b in (("candidate", "system", "candidate"),
                                ("aa_system", "system", "system_aa"),
                                ("aa_candidate", "candidate", "candidate_aa")):
                values = [x / y for x, y in zip(groups[a], groups[b])]
                ratios[metric][label] = dict(median=statistics.median(values), minimum=min(values),
                    maximum=max(values), above_one=sum(v > 1 for v in values), rounds=values)
        result.append(dict(name=name, workers=workers, ratios=ratios, absolute=absolute))
    return dict(processes=len(records), cells=result)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("phase", choices=("worker", "verify", "bench", "confirm"))
    for name in ("inputs", "system", "candidate", "output", "source", "data", "snapshot"):
        parser.add_argument("--" + name, type=pathlib.Path)
    parser.add_argument("--mode", choices=("system", "private"))
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--warmup", type=int, default=500)
    parser.add_argument("--samples", type=int, default=500)
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    if args.phase == "worker":
        worker(args)
    else:
        execute(args)
