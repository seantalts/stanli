#!/usr/bin/env python3
"""Matched native shared-library gradients, loaded after Python/NumPy.

verify creates the calibration and full-byte baseline; bench uses it unchanged.
All execution phases refuse existing output directories. This is not a
replacement for the CLI/CmdStan benchmark boundary.
"""
import argparse
import ctypes
import hashlib
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

ROOT = pathlib.Path(__file__).resolve().parents[2]
EIGHT = {"normal_8", "normal_1024", "normal_262144", "gamma_16384",
         "eight_schools_noncentered", "hierarchical_gp"}


def sha(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def save(path, value):
    pathlib.Path(path).write_text(json.dumps(value, indent=2) + "\n")


def worker(args):
    import numpy as np
    old = np.arange(8192, dtype=np.float64)
    library = ctypes.CDLL(str(args.library.resolve()))
    owns = library.stanli_allocator_bench_owns
    owns.argtypes, owns.restype = [ctypes.c_void_p], ctypes.c_bool
    fresh = np.arange(8192, dtype=np.float64)
    assert not owns(old.ctypes.data) and not owns(fresh.ctypes.data)
    library.stanli_build_id.restype = ctypes.c_char_p
    print(json.dumps(dict(kind="host", numpy=np.__version__,
                         build_id=library.stanli_build_id().decode())), flush=True)
    def host_state(stage):
        if not args.host_diagnostics:
            return
        # Outside native timing. Retain raw thread CPU counters and cgroup
        # limits rather than inferring contention from elapsed time alone.
        tasks = {}
        for path in pathlib.Path("/proc/self/task").glob("*/stat"):
            try:
                tasks[path.parent.name] = path.read_text()
            except FileNotFoundError:
                pass
        cgroup = {}
        for name in ("cpu.max", "cpu.stat", "cpuset.cpus.effective"):
            path = pathlib.Path("/sys/fs/cgroup") / name
            if path.exists():
                cgroup[name] = path.read_text()
        print(json.dumps(dict(kind="host_state", stage=stage, tasks=tasks,
                              cgroup=cgroup, at=time.time())), flush=True)
    host_state("loaded")
    if args.settle_ms:
        time.sleep(args.settle_ms / 1000)
    host_state("before_native")
    entry = library.stanli_allocator_bench_run
    entry.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_char_p)]
    entry.restype = ctypes.c_int
    argv = [b"allocator-bench", str(args.mir).encode(), str(args.data).encode(),
            args.mode.encode(), str(args.workers).encode(), str(args.reps).encode(),
            str(args.samples).encode(), str(args.cycles).encode(), b"recorded",
            str(args.snapshot).encode()]
    native = (ctypes.c_char_p * len(argv))(*argv)
    result = entry(len(argv), native)
    host_state("after_native")
    assert np.array_equal(old, fresh)
    assert not owns(old.ctypes.data) and not owns(fresh.ctypes.data)
    return result


def experiment(args):
    out = args.output.resolve()
    phase = out / args.phase
    phase.mkdir(parents=True)
    (phase / "logs").mkdir()
    cases = json.loads(args.inputs.read_text())["cases"]
    for case in cases:
        for name, digest in case["sha256"].items():
            assert sha(pathlib.Path(case["input_dir"]) / name) == digest
    variants = dict(system=dict(library=str(args.system.resolve()), mode="system"),
                    candidate=dict(library=str(args.candidate.resolve()), mode="private"))
    for variant in variants.values():
        variant["sha256"] = sha(variant["library"])
    slots = ["system", "candidate", "system_aa", "candidate_aa"]
    aliases = {s: s.removesuffix("_aa") for s in slots}
    by_name = {case["name"]: case for case in cases}
    cells = [[c["name"], w] for c in cases for w in (1, 4)]
    if args.eight:
        assert EIGHT <= by_name.keys()
        cells += [[name, 8] for name in sorted(EIGHT)]
    if args.cells:
        cells = json.loads(args.cells.read_text())
    assert len({tuple(c) for c in cells}) == len(cells)
    manifest = dict(phase=args.phase, head=subprocess.check_output(
        # actions/checkout's container mount can have a different owner. Trust
        # only this explicit checkout for this read; no global Git mutation.
        ["git", "-c", "safe.directory=" + str(ROOT), "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        machine=platform.platform(), processor=platform.processor(), python=sys.executable,
        input_sha256=sha(args.inputs), harness_sha256=sha(__file__),
        native_source_sha256=sha(ROOT / "tools/allocator/gradient_bench.cpp"),
        variants=variants, slots=slots, cells=cells, rounds=args.rounds, started=time.time())
    save(phase / "manifest.json", manifest)
    index = (phase / "runs.jsonl").open("x")
    count = 0
    env = {k: v for k, v in os.environ.items() if not k.startswith(
           ("DYLD_", "MIMALLOC_", "TCMALLOC_", "Malloc", "STANLI_"))}
    env["PYTHONDONTWRITEBYTECODE"] = "1"

    def one(cell, slot, reps, samples, rnd=-1):
        nonlocal count
        name, workers = cell
        variant = variants[aliases[slot]]
        label = f"{count:04d}-{name}-w{workers}-{slot}"
        count += 1
        base = phase / "logs" / label
        command = [sys.executable, __file__, "worker", "--library", variant["library"],
            "--mode", variant["mode"], "--mir", str(pathlib.Path(by_name[name]["input_dir"]) / "model.mir"),
            "--data", str(pathlib.Path(by_name[name]["input_dir"]) / "data.json"),
            "--workers", str(workers), "--reps", str(reps), "--samples", str(samples),
            "--snapshot", str(base.with_suffix(".snapshot"))]
        start = time.time()
        proc = subprocess.run(command, env=env, capture_output=True, text=True, timeout=180)
        base.with_suffix(".stdout").write_text(proc.stdout)
        base.with_suffix(".stderr").write_text(proc.stderr)
        rows = [json.loads(x) for x in proc.stdout.splitlines() if x.startswith("{")]
        timing = [r["wall_ns"] / reps / workers for r in rows if r.get("kind") == "timing"]
        graph = next((r for r in rows if r.get("kind") == "prepare"), {})
        graph = {k: v for k, v in graph.items() if k not in ("kind", "cycle", "ns")}
        snapshots = base.with_suffix(".snapshot")
        record = dict(name=name, workers=workers, slot=slot, round=rnd, reps=reps,
            command=command, returncode=proc.returncode, started=start, finished=time.time(),
            timing_ns=timing, median_ns=stats.median(timing) if timing else None, graph=graph,
            peak_rss_bytes=max((r["peak_rss_bytes"] for r in rows if r.get("kind") == "memory"), default=0),
            snapshot_sha256=sha(snapshots) if snapshots.exists() else None)
        index.write(json.dumps(record) + "\n")
        index.flush()
        assert proc.returncode == 0 and len(timing) == samples and graph, record
        return record

    if args.phase == "verify":
        calibration = []
        for cell in cells:
            baseline = one(cell, "system", 1024, 2)
            candidate = one(cell, "candidate", 128, 1)
            assert baseline["snapshot_sha256"] == candidate["snapshot_sha256"], cell
            assert baseline["graph"] == candidate["graph"], cell
            reps = max(64, min(4000000, 8 * math.ceil(150e6 / baseline["median_ns"] / cell[1] / 8)))
            calibration.append(dict(cell=cell, reps=reps, graph=baseline["graph"],
                                    snapshot_sha256=baseline["snapshot_sha256"]))
            print("verified", cell, "reps", reps, flush=True)
        save(out / "calibration.json", dict(manifest=manifest, cells=calibration))
    else:
        calibration = json.loads((out / "calibration.json").read_text())
        assert calibration["manifest"]["variants"] == variants, "Binaries changed since calibration"
        assert calibration["manifest"]["input_sha256"] == sha(args.inputs)
        assert calibration["manifest"]["harness_sha256"] == sha(__file__)
        assert calibration["manifest"]["native_source_sha256"] == manifest["native_source_sha256"]
        lookup = {tuple(c["cell"]): c for c in calibration["cells"]}
        schedule = []
        for rnd in range(args.rounds):
            shuffled = cells[:]
            random.Random(79201 + rnd).shuffle(shuffled)
            rotated = slots[rnd % len(slots):] + slots[:rnd % len(slots)]
            for cell in shuffled:
                for slot in rotated + rotated[::-1]:
                    schedule.append(dict(cell=cell, slot=slot, round=rnd))
        save(phase / "schedule.json", schedule)
        for i, step in enumerate(schedule):
            frozen = lookup[tuple(step["cell"])]
            result = one(step["cell"], step["slot"], frozen["reps"], 3, step["round"])
            assert result["snapshot_sha256"] == frozen["snapshot_sha256"], result
            assert result["graph"] == frozen["graph"], result
            if (i + 1) % (2 * len(slots)) == 0:
                print("round", step["round"] + 1, step["cell"], count, flush=True)
    index.close()
    save(phase / "completion.json", dict(processes=count, finished=time.time()))
    if args.phase == "bench":
        analyze(out)


def analyze(out):
    phase = out / "bench"
    manifest = json.loads((phase / "manifest.json").read_text())
    rows = [json.loads(x) for x in (phase / "runs.jsonl").read_text().splitlines()]
    assert len(rows) == len(manifest["cells"]) * 8 * manifest["rounds"]
    result = []
    for name, workers in manifest["cells"]:
        selected = [r for r in rows if (r["name"], r["workers"]) == (name, workers)]
        assert len({r["snapshot_sha256"] for r in selected}) == 1
        rounds, absolute, memory = {}, {}, {}
        for slot in manifest["slots"]:
            group = [r for r in selected if r["slot"] == slot]
            absolute[slot] = stats.median(r["median_ns"] for r in group)
            memory[slot] = stats.median(r["peak_rss_bytes"] for r in group)
            rounds[slot] = [stats.median(r["median_ns"] for r in group if r["round"] == rnd)
                            for rnd in range(manifest["rounds"])]
        ratios = {}
        for label, a, b in [("candidate", "system", "candidate"),
                            ("aa_system", "system", "system_aa"),
                            ("aa_candidate", "candidate", "candidate_aa")]:
            values = [x / y for x, y in zip(rounds[a], rounds[b])]
            ratios[label] = dict(median=stats.median(values), minimum=min(values),
                                 maximum=max(values), above_one=sum(x > 1 for x in values), rounds=values)
        result.append(dict(name=name, workers=workers, median_ns=absolute,
                           median_peak_rss_bytes=memory, ratios=ratios))
    save(phase / "summary.json", dict(processes=len(rows), cells=result))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("phase", choices=["worker", "verify", "bench", "analyze"])
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--inputs", type=pathlib.Path)
    parser.add_argument("--system", type=pathlib.Path)
    parser.add_argument("--candidate", type=pathlib.Path)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--eight", action="store_true")
    parser.add_argument("--cells", type=pathlib.Path)
    parser.add_argument("--library", type=pathlib.Path)
    parser.add_argument("--mir", type=pathlib.Path)
    parser.add_argument("--data", type=pathlib.Path)
    parser.add_argument("--mode", choices=["system", "private"])
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--reps", type=int, default=128)
    parser.add_argument("--samples", type=int, default=1)
    parser.add_argument("--cycles", type=int, default=1)
    parser.add_argument("--snapshot", type=pathlib.Path)
    parser.add_argument("--settle-ms", type=int, default=0)
    parser.add_argument("--host-diagnostics", action="store_true")
    args = parser.parse_args()
    if args.phase == "worker":
        sys.exit(worker(args))
    elif args.phase == "analyze":
        analyze(args.output.resolve())
    else:
        experiment(args)
