#!/usr/bin/env python3
"""Bounded compiler comparison; production sources and timing driver are unchanged."""
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time

from corpus_inventory import corpus_cases, materialize_data, source_digest
from verify_refs import ulp_distance

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "compiler-results"
BASE = "f1face395bfe9511a81fa2bb8c53decd3bb9b4a0"
MODELS = ("normal_mixture", "eight_schools_noncentered", "garch11",
          "logistic_regression_rhs", "sw_asymlaplace", "s2_zi_asymlaplace", "s2_gev")
PAIRS = 6


def run(args, **kwargs):
    try:
        return subprocess.run([str(x) for x in args], check=True, text=True,
                              capture_output=True, timeout=180, cwd=ROOT, **kwargs)
    except subprocess.CalledProcessError as error:
        print(error.stdout, end="", file=sys.stderr)
        print(error.stderr, end="", file=sys.stderr)
        raise


def stats(values):
    median = statistics.median(values)
    return {"median": median,
            "mad": statistics.median(abs(v - median) for v in values)}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    OUT.mkdir(exist_ok=True)
    run(["git", "diff", "--exit-code", BASE, "HEAD", "--",
         "runtime", "compiler", "CMakeLists.txt", "tools/bench_grad.cpp",
         "tools/benchmark_timer.hpp", "deps/fetch.sh"])
    affinity = sorted(os.sched_getaffinity(0))
    os.sched_setaffinity(0, {affinity[0]})
    record = {"base_commit": BASE, "experiment_commit": run(["git", "rev-parse", "HEAD"]).stdout.strip(),
              "platform": platform.platform(), "cpu": affinity[0], "available_cpus": affinity,
              "pairs": PAIRS, "warmup_ms": 200, "measure_ms": 250,
              "ratio": "Clang time / GCC time; below one favors Clang",
              "scope": "warm gradients, MIR preparation, process peak RSS and library size; no sampling or first-gradient timing",
              "compilers": {}, "models": {}, "aa_control": []}
    for compiler, command in (("gcc", "c++"), ("clang", "clang++")):
        build = ROOT / f"build-{compiler}"
        assert run([build / "stanli_check", "--compiler"]).stdout.strip() == compiler
        library = build / "libstanli.so"
        record["compilers"][compiler] = {
            "version": run([command, "--version"]).stdout,
            "bench_sha256": digest(build / "bench_grad"),
            "library_sha256": digest(library), "library_bytes": library.stat().st_size,
            "library_gzip_bytes": len(gzip.compress(library.read_bytes(), mtime=0)),
            "runtime_flags": (build / "CMakeFiles/stanli_runtime_objects.dir/flags.make").read_text(),
            "benchmark_flags": (build / "CMakeFiles/bench_grad.dir/flags.make").read_text()}
        oracle = run([sys.executable, ROOT / "tools/verify_refs.py", ROOT / "deps/posteriordb",
                      *MODELS, "--check", build / "stanli_check", "--jobs", "1", "--per-model"])
        (OUT / f"oracle-{compiler}.log").write_text(oracle.stdout + oracle.stderr)

    def measure(compiler, mir, data, prefix, prep=False):
        rss = OUT / f"{prefix}.rss-kib"
        args = ["/usr/bin/time", "-f", "%M", "-o", rss,
                ROOT / f"build-{compiler}/bench_grad", mir, data,
                "--prep" if prep else "--timed"]
        result = run(args)
        (OUT / f"{prefix}.stdout").write_text(result.stdout)
        (OUT / f"{prefix}.stderr").write_text(result.stderr)
        answer = ({"seconds": float(result.stdout.split()[0]),
                   "parameters": int(result.stdout.split()[1])}
                  if prep else json.loads(result.stdout))
        answer["rss_kib"] = int(rss.read_text())
        if not prep:
            answer["ns_per_gradient"] = answer["elapsed_ns"] / answer["iterations"]
        return answer

    cases = corpus_cases(ROOT / "deps/posteriordb")
    for name in MODELS:
        case = cases[name]
        data = materialize_data(case, OUT)
        mir = OUT / f"{name}.mir"
        start = time.monotonic_ns()
        run([ROOT / "deps/stanc3/stanli-vectorize-probe", "--vectorize-loops", "on",
             "--output", mir, case.source])
        entry = {"source_sha256": source_digest(case.source), "data_sha256": source_digest(data),
                 "mir_sha256": digest(mir), "source_to_mir_ns": time.monotonic_ns() - start,
                 "pairs": [], "prep_pairs": []}
        for pair in range(PAIRS):
            order = ("gcc", "clang") if pair % 2 == 0 else ("clang", "gcc")
            samples = {c: measure(c, mir, data, f"{name}-{pair}-{c}") for c in order}
            a, b = samples["gcc"]["values"], samples["clang"]["values"]
            assert len(a) == len(b) and all(math.isfinite(v) for v in a + b)
            relative = max(abs(x-y) / max(1, abs(x), abs(y)) for x, y in zip(a, b))
            assert relative <= 1e-9, (name, relative)
            samples["max_scaled_error"] = relative
            samples["max_ulp"] = max(ulp_distance(x, y) for x, y in zip(a, b))
            samples["ratio"] = samples["clang"]["ns_per_gradient"] / samples["gcc"]["ns_per_gradient"]
            entry["pairs"].append(samples)
            entry["prep_pairs"].append({c: measure(c, mir, data, f"{name}-{pair}-{c}-prep", True)
                                         for c in reversed(order)})
        entry["warm_ratio"] = stats([p["ratio"] for p in entry["pairs"]])
        for compiler in ("gcc", "clang"):
            entry[compiler] = {
                "warm_ns": stats([p[compiler]["ns_per_gradient"] for p in entry["pairs"]]),
                "prep_seconds": stats([p[compiler]["seconds"] for p in entry["prep_pairs"]]),
                "rss_kib": stats([p[compiler]["rss_kib"] for p in entry["pairs"]])}
        record["models"][name] = entry
        (OUT / "report.json").write_text(json.dumps(record, indent=2) + "\n")
        print(name, json.dumps(entry["warm_ratio"]), flush=True)
        if name == "normal_mixture":
            for pair in range(PAIRS):
                order = ("a", "b") if pair % 2 == 0 else ("b", "a")
                control = {label: measure("clang", mir, data, f"aa-{pair}-{label}") for label in order}
                record["aa_control"].append({**control, "ratio": control["b"]["ns_per_gradient"] / control["a"]["ns_per_gradient"]})
    record["aa_ratio"] = stats([p["ratio"] for p in record["aa_control"]])
    record["geomean_warm_ratio"] = math.exp(statistics.mean(math.log(e["warm_ratio"]["median"])
                                                         for e in record["models"].values()))
    (OUT / "report.json").write_text(json.dumps(record, indent=2) + "\n")
    print("geomean", record["geomean_warm_ratio"], "A/A", record["aa_ratio"], flush=True)


if __name__ == "__main__":
    main()
