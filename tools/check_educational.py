#!/usr/bin/env python3
"""Educational corpus: strict CmdStan reference replay and complete CLI sampling.

--record regenerates references from CmdStan independently of Stanli results.
--benchmark runs paired, alternating CLI samples and enforces CmdStan/Stanli
median wall time >= 0.5 for every model. CmdStan compilation is reported
separately; its cost cannot hide a slow Stanli sampler or generated quantities.
"""
import argparse
import csv
import gzip
import hashlib
import io
import json
import math
import os
import pathlib
import platform
import re
import statistics
import subprocess
import sys
import time

from check_function_models import digest, parse, source_digest
from cmdstan_ref import compile_cmd

REPO = pathlib.Path(__file__).resolve().parents[1]
CORPUS = REPO / "tests/educational"
REFERENCE = CORPUS / "references.json.gz"


def run(argv, timeout=600, cwd=REPO, stdout=None):
    start = time.perf_counter()
    result = subprocess.run([str(x) for x in argv], cwd=cwd, text=True,
                            stdout=stdout or subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=timeout)
    elapsed = time.perf_counter() - start
    if result.returncode:
        raise RuntimeError(f"{argv[0]} exited {result.returncode}: "
                           + (result.stdout or "")[-2000:] + result.stderr[-2000:])
    return result, elapsed


def inventory():
    entries = json.loads((CORPUS / "manifest.json").read_text())["models"]
    names = [e["model_id"] for e in entries]
    actual = {p.name for p in (CORPUS / "models").iterdir() if p.is_dir()}
    if len(names) != 13 or len(set(names)) != 13 or set(names) != actual:
        raise ValueError("Expected exactly the 13 manifested educational models")
    for entry in entries:
        for key, sha in (("model_path", "sha256"), ("data_path", "data_sha256")):
            if source_digest(CORPUS / entry[key]) != entry[sha]:
                raise ValueError(f"Imported fixture changed: {entry[key]}")
    return sorted(names)


def files(name):
    root = CORPUS / "models" / name
    return root / "model.stan", root / "data.json"


def binary(args, name):
    return args.build / (name + (".exe" if os.name == "nt" else ""))


def toolchain(args):
    result = {"platform": platform.platform(), "stanc_sha256": digest(args.stanc),
              "compiler": run(["clang++", "--version"])[0].stdout.splitlines()[0],
              "reference_flags": "-O3 -ffp-contract=off; stanc --O1"}
    for key, path in (("cmdstan", args.cmdstan), ("stan", args.cmdstan / "stan"),
                      ("math", args.cmdstan / "stan/lib/stan_math")):
        result[key] = run(["git", "-C", path, "rev-parse", "HEAD"])[0].stdout.strip()
    return result


def record(names, args):
    rig = toolchain(args)
    models = {}
    for name in names:
        source, data = files(name)
        identity = hashlib.sha256((source_digest(source) + source_digest(REPO / "tools/ref_driver.cpp")
                                  + json.dumps(rig, sort_keys=True)).encode()).hexdigest()[:16]
        work = args.build / "educational-refs" / (name + "-" + identity)
        work.mkdir(parents=True, exist_ok=True)
        hpp, exe = work / "model.hpp", work / "ref"
        if not exe.exists():
            run([args.stanc, source, "--O1", f"--o={hpp}"])
            run(compile_cmd(args.cmdstan, hpp, REPO / "tools/ref_driver.cpp", exe,
                            opt="-O3", sundials=False))
        points = [parse(run([exe, data, point])[0].stdout) for point in range(3)]
        info = json.loads(run([args.stanc, "--info", source])[0].stdout)
        parameter_names = [n for n in points[0]["names"]
                           if n.split(".")[0] in info["parameters"]]
        models[name] = {"source_sha256": source_digest(source),
                        "data_sha256": source_digest(data), "points": points,
                        "parameter_names": parameter_names}
        print("RECORDED", name, flush=True)
    payload = json.dumps({"schema": 1, "toolchain": rig, "models": models},
                         sort_keys=True, allow_nan=False).encode()
    REFERENCE.write_bytes(gzip.compress(payload, mtime=0))


def compare(want, got):
    if got["names"] != want["names"]:
        raise ValueError("Output names/order differ")
    worst = 0.0
    for field in ("lp_grad", "values"):
        if len(want[field]) != len(got[field]):
            raise ValueError(field + " shape differs")
        for i, (a, b) in enumerate(zip(want[field], got[field])):
            if not math.isfinite(a) or not math.isfinite(b):
                raise ValueError(field + " contains nonfinite values")
            error = abs(a-b) / max(1.0, abs(a), abs(b))
            worst = max(worst, error)
            if error > 1e-9:
                raise ValueError(f"{field}[{i}]: expected {a}, got {b}; scaled error {error}")
    return worst


def sample_csv(text, names, samples):
    rows = list(csv.reader(line for line in io.StringIO(text)
                           if line.strip() and not line.startswith("#")))
    if not rows or len(rows) != samples + 1:
        raise ValueError(f"Expected {samples} draws, got {max(0, len(rows)-1)}")
    header = rows[0]
    outputs = [n for n in header if not n.endswith("__")]
    if outputs != names or len(set(header)) != len(header):
        raise ValueError("Sample CSV output names/order differ from CmdStan")
    result = []
    for row in rows[1:]:
        if len(row) != len(header):
            raise ValueError("Ragged sample CSV")
        values = [float(x) for x in row]
        if not all(math.isfinite(x) for x in values):
            raise ValueError("Nonfinite sample, diagnostic, or generated quantity")
        result.append(dict(zip(header, values)))
    return result


def stanli_command(args, source, data, seed, warmup, samples):
    return [binary(args, "stanli_run"), source, data, "--seed", seed,
            "--warmup", warmup, "--samples", samples, "--init-radius", 0,
            "--sampler-stats", "--timings"]


def parse_timings(stderr):
    match = re.search(r"stanli_run: timings prep_s=(\S+) sample_s=(\S+) output_s=(\S+)", stderr)
    if not match:
        raise ValueError("Missing phase timing output")
    result = dict(zip(("prep_s", "sample_s", "output_s"), map(float, match.groups())))
    if not all(math.isfinite(x) and x >= 0 for x in result.values()):
        raise ValueError("Invalid phase timing")
    return result


def speed_gate(measurements):
    if set(measurements) != {"stanli", "cmdstan"}:
        raise ValueError("Both engines must be measured")
    if len(measurements["stanli"]) != len(measurements["cmdstan"]):
        raise ValueError("Timing repetitions must be paired")
    for values in measurements.values():
        if len(values) < 3 or not all(math.isfinite(x) and x > 0 for x in values):
            raise ValueError("Need >=3 finite, positive timings per engine")
    medians = {engine: statistics.median(values) for engine, values in measurements.items()}
    dispersion = {engine: {"min": min(values), "max": max(values),
                           "mad": statistics.median(abs(x-medians[engine]) for x in values)}
                  for engine, values in measurements.items()}
    speedup = medians["cmdstan"] / medians["stanli"]
    return {"seconds": measurements, "medians": medians, "dispersion": dispersion,
            "speedup": speedup, "pass": speedup >= 0.5}


def posterior_gate(chains, names):
    """Compare parameter means using batch-means Monte Carlo uncertainty.

    Twenty contiguous batches per chain retain within-chain correlation;
    the between-chain estimate is a lower bound on the uncertainty too.
    Six combined standard errors is a broad regression test, not evidence
    of convergence or an assertion that two random trajectories must match.
    """
    result = {}
    for name in names:
        summaries = {}
        for engine, runs in chains.items():
            batches, means = [], []
            for rows in runs:
                values = [r[name] for r in rows]
                size = len(values) // 20
                if size < 2:
                    raise ValueError("Posterior comparison needs at least 40 samples")
                batches.extend(statistics.mean(values[i*size:(i+1)*size]) for i in range(20))
                means.append(statistics.mean(values))
            se = max(math.sqrt(statistics.variance(batches) / len(batches)),
                     math.sqrt(statistics.variance(means) / len(means)))
            summaries[engine] = {"mean": statistics.mean(means), "mcse": se}
        a, b = summaries["stanli"], summaries["cmdstan"]
        limit = 6 * math.hypot(a["mcse"], b["mcse"]) + 1e-9 * max(1, abs(a["mean"]), abs(b["mean"]))
        result[name] = {**summaries, "difference": abs(a["mean"]-b["mean"]),
                        "limit": limit, "pass": abs(a["mean"]-b["mean"]) <= limit}
    if not result:
        raise ValueError("No parameter columns for posterior comparison")
    return result


def replay(name, reference, args):
    source, data = files(name)
    if reference["source_sha256"] != source_digest(source) or reference["data_sha256"] != source_digest(data):
        raise ValueError("Reference source/data hashes differ")
    if len(reference["points"]) != 3:
        raise ValueError("Expected all three reference points")
    worst = 0.0
    for point, want in enumerate(reference["points"]):
        proc, _ = run([binary(args, "stanli_check"), source, data,
                       "--point", point, "--wa-values"])
        worst = max(worst, compare(want, parse(proc.stdout)))
    proc, elapsed = run(stanli_command(args, source, data, 42, 100, 100))
    sample_csv(proc.stdout, reference["points"][0]["names"], 100)
    print(f"PASS {name}: 3 points/all outputs, 100 draws; error={worst:.3g}", flush=True)
    return {"max_scaled_error": worst, "smoke_seconds": elapsed}


def benchmark(name, reference, args):
    source, data = files(name)
    work = args.output.parent / name
    work.mkdir(parents=True, exist_ok=True)
    # Build fresh each invocation, without changing the shared CmdStan stanc.
    model = work / (name + ".stan")
    model.write_bytes(source.read_bytes())
    exe = work / name
    exe.unlink(missing_ok=True)
    _, build_seconds = run(["make", exe, "STANCFLAGS=--O1", "O=3"], cwd=args.cmdstan)
    measurements = {"stanli": [], "cmdstan": []}
    chains = {"stanli": [], "cmdstan": []}
    phases = []
    output_names = reference["points"][0]["names"]
    # Untimed warmup run followed by alternating paired repetitions.
    for repetition in range(args.repetitions + 1):
        for engine in (("stanli", "cmdstan") if repetition % 2 == 0 else ("cmdstan", "stanli")):
            if engine == "stanli":
                command = stanli_command(args, source, data, repetition + 1, args.warmup, args.samples)
            else:
                command = [exe, "sample", f"num_warmup={args.warmup}", f"num_samples={args.samples}",
                           "random", f"seed={repetition+1}", "init=0", "data", f"file={data}",
                           "output", f"file={work / 'cmdstan.csv'}", "sig_figs=17", "refresh=0"]
            output = work / f"{engine}-{repetition}.csv"
            if engine == "stanli":
                # Both engines write CSV files inside the timed interval.
                with output.open("w") as stream:
                    proc, elapsed = run(command, timeout=args.timeout, stdout=stream)
                text = output.read_text()
            else:
                proc, elapsed = run(command, timeout=args.timeout)
                text = (work / "cmdstan.csv").read_text()
            rows = sample_csv(text, output_names, args.samples)
            output.write_text(text)
            (work / f"{engine}-{repetition}.stderr").write_text(proc.stderr)
            if repetition:
                measurements[engine].append(elapsed)
                chains[engine].append(rows)
                if engine == "stanli":
                    phases.append(parse_timings(proc.stderr))
    result = speed_gate(measurements)
    result["cmdstan_build_seconds"] = build_seconds
    result["stanli_phases"] = phases
    result["cmdstan_binary_sha256"] = digest(exe)
    result["cmdstan_stanc_sha256"] = digest(args.cmdstan / "bin/stanc")
    result["posterior"] = posterior_gate(chains, reference["parameter_names"])
    result["posterior_pass"] = all(p["pass"] for p in result["posterior"].values())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build-rel")
    parser.add_argument("--cmdstan", type=pathlib.Path, default=REPO / "deps/cmdstan")
    parser.add_argument("--stanc", type=pathlib.Path, default=REPO / "deps/stanc3/stanc")
    parser.add_argument("--record", action="store_true")
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--output", type=pathlib.Path, default=REPO / "build-rel/educational/results.json")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=1000)
    parser.add_argument("--samples", type=int, default=1000)
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    for key in ("build", "cmdstan", "stanc", "output"):
        setattr(args, key, getattr(args, key).resolve())
    if args.repetitions < 3 or args.warmup < 1 or args.samples < 40:
        parser.error("Need >=3 repetitions, positive warmup, and >=40 samples")
    if args.benchmark:
        cache = (args.build / "CMakeCache.txt").read_text()
        if "CMAKE_BUILD_TYPE:STRING=Release" not in cache:
            parser.error("Performance measurements require a Release build")
    names = inventory()
    if args.record:
        record(names, args)
    refs = json.loads(gzip.decompress(REFERENCE.read_bytes()))
    if refs.get("schema") != 1 or set(refs["models"]) != set(names):
        raise ValueError("Reference schema or model inventory differs")
    report = {"models": {}, "settings": {k: str(v) if isinstance(v, pathlib.Path) else v
                                          for k, v in vars(args).items()},
              "stanli_check_sha256": digest(binary(args, "stanli_check")),
              "stanli_run_sha256": digest(binary(args, "stanli_run")),
              "reference_sha256": digest(REFERENCE), "platform": platform.platform()}
    if args.benchmark:
        report["toolchain"] = toolchain(args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    failures = []
    for name in names:
        try:
            result = replay(name, refs["models"][name], args)
            report["models"][name] = result
            if args.benchmark:
                result["benchmark"] = benchmark(name, refs["models"][name], args)
                print(f"SPEED {name}: {result['benchmark']['speedup']:.3f}x CmdStan", flush=True)
                if not result["benchmark"]["posterior_pass"]:
                    raise ValueError("Sampled parameter means differ beyond Monte Carlo uncertainty")
                if not result["benchmark"]["pass"]:
                    raise ValueError("Stanli below 0.5x CmdStan end-to-end speed")
        except (ValueError, RuntimeError, subprocess.TimeoutExpired, OSError) as error:
            failures.append(name)
            report["models"].setdefault(name, {})["error"] = str(error)
            print(f"FAIL {name}: {error}", flush=True)
        args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    print(f"{len(names)-len(failures)}/{len(names)} educational models passed", flush=True)
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
