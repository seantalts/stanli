#!/usr/bin/env python3
"""Bounded diagnostic: same DSO, different out-of-loop worker allocation sites."""
import argparse
import json
import math
import os
import pathlib
import platform
import random
import statistics as stats
import subprocess
import sys
import time

from bench import ROOT, save, sha

CELLS = [("normal_8", 1), ("normal_8", 4), ("eight_schools_noncentered", 4),
         ("normal_1024", 4), ("normal_262144", 4)]


def experiment(args):
    out = args.output.resolve()
    out.mkdir(parents=True)
    (out / "logs").mkdir()
    inputs = json.loads(args.inputs.read_text())
    cases = {c["name"]: c for c in inputs["cases"]}
    # CI artifacts retain original absolute paths. Relocate only after checking
    # every original source/data/MIR hash; never silently regenerate the corpus.
    paths = {}
    for name, case in cases.items():
        path = args.inputs.resolve().parent / "inputs" / name
        if not path.exists():
            path = pathlib.Path(case["input_dir"])
        for filename, digest in case["sha256"].items():
            assert sha(path / filename) == digest, (name, filename)
        paths[name] = path
    design = json.loads(args.design.read_text()) if args.design else None
    assert design or (args.system and args.candidate), "Supply a design or both libraries"
    variants = {} if design else {mode + "-" + place: dict(library=str(library.resolve()),
                    mode=mode, placement=place, sha256=sha(library))
                for mode, library in [("system", args.system), ("private", args.candidate)]
                for place in ["main", "gradient", "worker"]}
    cells_for_run = [tuple(c) for c in design["cells"]] if design else CELLS
    baseline_slot = design["baseline"] if design else "system-main"
    if design:
        variants = design["variants"]
        for variant in variants.values():
            variant["library"] = str(pathlib.Path(variant["library"]).resolve())
            variant["sha256"] = sha(variant["library"])
        assert baseline_slot in variants
    slots = list(variants) + [name + "_aa" for name in variants]
    manifest = dict(head=subprocess.check_output(["git", "-c", "safe.directory=" + str(ROOT),
        "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(), variants=variants,
        cells=cells_for_run, slots=slots, rounds=5, input_sha256=sha(args.inputs),
        design_sha256=sha(args.design) if args.design else None,
        harness_sha256=sha(__file__), worker_sha256=sha(ROOT / "tools/allocator/bench.py"),
        native_sha256=sha(ROOT / "tools/allocator/gradient_bench.cpp"),
        platform=platform.platform(), started=time.time())
    save(out / "manifest.json", manifest)
    env = {k: v for k, v in os.environ.items() if not k.startswith(
        ("DYLD_", "MIMALLOC_", "TCMALLOC_", "Malloc", "STANLI_"))}
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    index = (out / "runs.jsonl").open("x")
    rows = []

    def one(cell, slot, reps, samples, round_number=-1):
        name, workers = cell
        variant = variants[slot.removesuffix("_aa")]
        base = out / "logs" / f"{len(rows):04d}-{name}-w{workers}-{slot}"
        command = [sys.executable, ROOT / "tools/allocator/bench.py", "worker",
            "--library", variant["library"], "--mode", variant["mode"],
            "--mir", paths[name] / "model.mir", "--data", paths[name] / "data.json",
            "--workers", workers, "--reps", reps, "--samples", samples,
            "--snapshot", base.with_suffix(".snapshot")]
        command = list(map(str, command))
        started = time.time()
        process = subprocess.run(command, env={**env,
            "STANLI_ALLOCATOR_BENCH_PLACEMENT": variant["placement"]},
            capture_output=True, text=True, timeout=180)
        base.with_suffix(".stdout").write_text(process.stdout)
        base.with_suffix(".stderr").write_text(process.stderr)
        data = [json.loads(line) for line in process.stdout.splitlines() if line.startswith("{")]
        timings = [r["wall_ns"] / workers / reps for r in data if r.get("kind") == "timing"]
        graph = next((r for r in data if r.get("kind") == "prepare"), {})
        graph = {k: v for k, v in graph.items() if k not in ("kind", "cycle", "ns")}
        record = dict(name=name, workers=workers, slot=slot, round=round_number,
            reps=reps, command=command, started=started, finished=time.time(),
            returncode=process.returncode, graph=graph, timing_ns=timings,
            median_ns=stats.median(timings) if timings else None,
            addresses=[r for r in data if r.get("kind") == "placement"],
            snapshot_sha256=sha(base.with_suffix(".snapshot")) if base.with_suffix(".snapshot").exists() else None)
        rows.append(record)
        index.write(json.dumps(record) + "\n")
        index.flush()
        assert process.returncode == 0 and len(timings) == samples, record
        assert len(record["addresses"]) == workers * 4, record
        return record

    calibration = {}
    for cell in cells_for_run:
        baseline = one(cell, baseline_slot, 1024, 2)
        for slot in variants:
            if slot == baseline_slot:
                continue
            result = one(cell, slot, 128, 1)
            assert result["snapshot_sha256"] == baseline["snapshot_sha256"]
            assert result["graph"] == baseline["graph"]
        reps = max(64, min(4000000, 8 * math.ceil(150e6 / baseline["median_ns"] / cell[1] / 8)))
        calibration[cell] = dict(reps=reps, graph=baseline["graph"], snapshot_sha256=baseline["snapshot_sha256"])
    save(out / "calibration.json", [dict(cell=cell, **row) for cell, row in calibration.items()])
    if args.verify_only:
        index.close()
        save(out / "verification-complete.json", dict(processes=len(rows), finished=time.time()))
        return
    schedule = []
    for rnd in range(5):
        cells = cells_for_run[:]
        random.Random(32629 + rnd).shuffle(cells)
        order = slots[rnd % len(slots):] + slots[:rnd % len(slots)]
        for cell in cells:
            for slot in order + order[::-1]:
                schedule.append(dict(cell=cell, slot=slot, round=rnd))
    save(out / "schedule.json", schedule)
    for i, step in enumerate(schedule):
        frozen = calibration[tuple(step["cell"])]
        row = one(step["cell"], step["slot"], frozen["reps"], 3, step["round"])
        assert row["snapshot_sha256"] == frozen["snapshot_sha256"] and row["graph"] == frozen["graph"]
        if (i + 1) % (2 * len(slots)) == 0:
            print("completed", i + 1, "of", len(schedule), flush=True)
    index.close()
    results = []
    for name, workers in cells_for_run:
        selected = [r for r in rows if (r["name"], r["workers"]) == (name, workers) and r["round"] >= 0]
        grouped = {slot: [stats.median(r["median_ns"] for r in selected if r["slot"] == slot and r["round"] == rnd)
                          for rnd in range(5)] for slot in slots}
        comparisons = {}
        if design:
            pairs = design["comparisons"]
        else:
            pairs = [(place, "system-" + place, "private-" + place) for place in ("main", "gradient", "worker")]
            pairs += [(mode + "-gradient/main", mode + "-main", mode + "-gradient") for mode in ("system", "private")]
            pairs += [(mode + "-worker/main", mode + "-main", mode + "-worker") for mode in ("system", "private")]
        pairs += [(slot + "-aa", slot, slot + "_aa") for slot in variants]
        for label, a, b in pairs:
            values = [x / y for x, y in zip(grouped[a], grouped[b])]
            comparisons[label] = dict(median=stats.median(values), minimum=min(values), maximum=max(values),
                                      above_one=sum(x > 1 for x in values), rounds=values)
        results.append(dict(name=name, workers=workers, ratios=comparisons))
    save(out / "summary.json", dict(cells=results, timing_processes=len(schedule), processes=len(rows)))
    save(out / "completion.json", dict(processes=len(rows), finished=time.time()))


if __name__ == "__main__":
    p = argparse.ArgumentParser(__doc__)
    p.add_argument("--inputs", type=pathlib.Path, required=True)
    p.add_argument("--system", type=pathlib.Path)
    p.add_argument("--candidate", type=pathlib.Path)
    p.add_argument("--design", type=pathlib.Path)
    p.add_argument("--output", type=pathlib.Path, required=True)
    p.add_argument("--verify-only", action="store_true")
    experiment(p.parse_args())
