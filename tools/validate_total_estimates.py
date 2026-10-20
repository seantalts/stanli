#!/usr/bin/env python3
"""Check the benchmark's total-sampling estimate against real sampling runs.

For each named model, run Stanli and CmdStan NUTS with the reference-run
settings (one chain, seed 1, 1000 warmup, 1000 draws, default adaptation,
warmup saved) and compare the sampler time each reports with the estimate:
the model's reference-run gradient count times the mean gradient time that
harnesses/corpus_bench.py measured at the ten reference points. Setup is left
out of both sides. It also compares the two engines' total n_leapfrog__.

    python3 tools/validate_total_estimates.py --lib build-points/libstanli.dylib \\
        --cmdstan deps/cmdstan --summary RUN.tsv --models arK,sw_gp

The summaries come from harnesses/corpus_bench.py runs that used the same
points file. The output is a measurement report; nothing here is published.
"""

import argparse
import csv
import json
import os
import pathlib
import re
import shutil
import statistics
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import benchmark_points as bp
import gen_benchmark_points as gen

REPO = pathlib.Path(__file__).resolve().parent.parent
STANC = REPO / "deps/stanc3/stanc"
THREAD_ENV = {k: "1" for k in ("STAN_NUM_THREADS", "OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS",
                               "MKL_NUM_THREADS", "VECLIB_MAXIMUM_THREADS")}


ELAPSED = re.compile(r"^#\s*(?:Elapsed Time:)?\s*([0-9.eE+-]+) seconds \((Warm-up|Sampling)\)")


def parse_cmdstan_csv(path):
    """Total n_leapfrog__ per iteration and the elapsed seconds CmdStan wrote into the CSV comments."""
    column, leapfrogs, seconds = None, [], {}
    for line in pathlib.Path(path).read_text().splitlines():
        if line.startswith("#"):
            match = ELAPSED.match(line)
            if match:
                seconds["warmup" if match.group(2) == "Warm-up" else "sampling"] = float(match.group(1))
            continue
        if not line.strip():
            continue
        if column is None:
            column = line.split(",").index("n_leapfrog__")
            continue
        leapfrogs.append(int(float(line.split(",")[column])))
    if column is None or len(seconds) != 2:
        raise ValueError(f"{path}: not a complete CmdStan sample CSV")
    return dict(n_leapfrog=leapfrogs, seconds=seconds)


def build_cmdstan(cmdstan, stanc, stan, workdir):
    workdir.mkdir(parents=True, exist_ok=True)
    name = stan.stem
    shutil.copyfile(stan, workdir / f"{name}.stan")
    subprocess.run(["make", str(workdir / name), f"STANC={stanc}", "STANCFLAGS="], cwd=cmdstan,
                   check=True, capture_output=True, text=True)
    return workdir / name


def run_cmdstan(exe, data, seed, warmup, draws, output, timeout):
    subprocess.run([str(exe), "sample", f"num_warmup={warmup}", f"num_samples={draws}", "save_warmup=1",
                    "algorithm=hmc", "engine=nuts", f"max_depth={gen.MAX_DEPTH}", "adapt", "engaged=1",
                    f"delta={gen.DELTA}", "random", f"seed={seed}", "id=1", "data", f"file={data}",
                    "output", f"file={output}", "refresh=0"],
                   check=True, capture_output=True, text=True, timeout=timeout,
                   env=dict(os.environ, **THREAD_ENV))
    return parse_cmdstan_csv(output)


def gradient_times(summaries):
    found = {}
    for path in summaries:
        with open(path, newline="") as stream:
            for row in csv.DictReader(stream, delimiter="\t"):
                if row.get("stanli_ns_grad") and row.get("cmdstan_ns_grad"):
                    found[row["model"]] = (float(row["stanli_ns_grad"]), float(row["cmdstan_ns_grad"]),
                                           row.get("gradient_count", ""))
    return found


def validate(models, cases, document, times, lib, probe, cmdstan, stanc, workdir, timeout):
    results = []
    for name in models:
        entry = document["models"][name]
        if entry["source"] != "reference_run":
            results.append(dict(model=name, error="fallback model: " + entry["reason"]))
            continue
        if name not in times:
            results.append(dict(model=name, error="no gradient times in the summaries"))
            continue
        stan, data = cases[name]
        settings = document["settings"]
        scratch = workdir / name
        scratch.mkdir(parents=True, exist_ok=True)
        mir = scratch / "model.sexp"
        subprocess.run([str(probe), "--vectorize-loops", "on", "--output", str(mir), str(stan)],
                       check=True, capture_output=True)
        row = dict(model=name, count=entry["gradients"]["total"], ns_grad=dict(zip(("stanli", "cmdstan"), times[name][:2])))
        stanli = gen.run_chain(lib, mir, data, settings["seed"], settings["warmup"], settings["draws"])
        exe = build_cmdstan(cmdstan, stanc, stan, scratch)
        reference = run_cmdstan(exe, data, settings["seed"], settings["warmup"], settings["draws"],
                                scratch / "output.csv", timeout)
        row["actual_s"] = dict(stanli=sum(stanli["seconds"].values()), cmdstan=sum(reference["seconds"].values()))
        row["estimated_s"] = {engine: row["count"] * row["ns_grad"][engine] / 1e9 for engine in row["ns_grad"]}
        row["leapfrog"] = dict(stanli=sum(stanli["n_leapfrog"]), cmdstan=sum(reference["n_leapfrog"]),
                               file=row["count"])
        results.append(row)
        print(f"{name}: done", file=sys.stderr, flush=True)
    return results


def render(results):
    lines = ["| Model | Gradients | Stanli est (s) | Stanli actual (s) | CmdStan est (s) | CmdStan actual (s) "
             "| Ratio est | Ratio actual | Stanli n_leapfrog | CmdStan n_leapfrog | CmdStan/Stanli leapfrog |",
             "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    ratios = []
    for r in results:
        if "error" in r:
            lines.append(f"| {r['model']} | {r['error']} |" + " |" * 9)
            continue
        e, a, g = r["estimated_s"], r["actual_s"], r["leapfrog"]
        est, act = e["cmdstan"] / e["stanli"], a["cmdstan"] / a["stanli"]
        ratios.append(act / est)
        lines.append(f"| {r['model']} | {r['count']} | {e['stanli']:.3g} | {a['stanli']:.3g} | {e['cmdstan']:.3g} "
                     f"| {a['cmdstan']:.3g} | {est:.3g} | {act:.3g} | {g['stanli']} | {g['cmdstan']} "
                     f"| {g['cmdstan'] / g['stanli']:.3f} |")
    if ratios:
        lines += ["", f"Median of (actual ratio / estimated ratio) over {len(ratios)} models: {statistics.median(ratios):.3g}"]
    return "\n".join(lines) + "\n"


def main():
    sys.path.insert(0, str(REPO / "tools"))
    from corpus_inventory import corpus_cases, materialize_data
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--lib", type=pathlib.Path, required=True)
    parser.add_argument("--cmdstan", type=pathlib.Path, default=REPO / "deps/cmdstan")
    parser.add_argument("--stanc", type=pathlib.Path, default=STANC)
    parser.add_argument("--probe", type=pathlib.Path, default=gen.PROBE)
    parser.add_argument("--pdb", type=pathlib.Path, default=REPO / "deps/posteriordb")
    parser.add_argument("--points-file", type=pathlib.Path, default=bp.DEFAULT_PATH)
    parser.add_argument("--summary", type=pathlib.Path, action="append", required=True,
                        help="corpus_bench summary TSV with gradient times (repeatable)")
    parser.add_argument("--models", required=True, help="comma-separated model names")
    parser.add_argument("--workdir", type=pathlib.Path)
    parser.add_argument("--json", type=pathlib.Path, help="also write the raw results here")
    parser.add_argument("--timeout", type=int, default=3600)
    args = parser.parse_args()
    document = bp.load(args.points_file)
    models = args.models.split(",")
    with tempfile.TemporaryDirectory(prefix="stanli-validate-") as temporary:
        workdir = (args.workdir or pathlib.Path(temporary)).absolute()
        all_cases = corpus_cases(args.pdb, "all")
        cases = {name: (all_cases[name].source, materialize_data(all_cases[name], workdir / "data"))
                 for name in models}
        results = validate(models, cases, document, gradient_times(args.summary), args.lib.absolute(),
                           args.probe.absolute(), args.cmdstan.absolute(), args.stanc.absolute(),
                           workdir, args.timeout)
    print(render(results), end="")
    if args.json:
        args.json.write_text(json.dumps(results, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
