#!/usr/bin/env python3
import argparse
import json
import pathlib
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import verify_refs  # noqa: E402
from corpus_inventory import corpus_cases  # noqa: E402


def run_bench(binary, mir, data, warmup, measure, timeout):
    try:
        p = subprocess.run(
            [str(binary), str(mir), str(data), "--timed", "--warmup-ms",
             str(warmup), "--measure-ms", str(measure)],
            capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "timeout"
    if p.returncode != 0:
        return None, f"exit {p.returncode}: {p.stderr.strip()[-160:]}"
    for line in reversed(p.stdout.splitlines()):
        if line.startswith("{"):
            try:
                return json.loads(line), ""
            except ValueError:
                break
    return None, "no json"


def max_ulp(a, b):
    if len(a) != len(b):
        return -1
    return max((verify_refs.pair_dev(x, y)[1] for x, y in zip(a, b)),
               default=0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", action="append", required=True,
                    metavar="NAME=BUILD_DIR")
    ap.add_argument("--pdb", type=pathlib.Path, required=True)
    ap.add_argument("--out", type=pathlib.Path, required=True)
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--warmup-ms", type=int, default=100)
    ap.add_argument("--measure-ms", type=int, default=200)
    ap.add_argument("--timeout", type=float, default=300)
    ap.add_argument("--budget-min", type=float, default=1e9)
    ap.add_argument("--shard", default="0/1")
    ap.add_argument("models", nargs="*")
    args = ap.parse_args()

    variants = []
    for spec in args.variant:
        name, _, path = spec.partition("=")
        variants.append((name, pathlib.Path(path).resolve()))
    base_name, base_dir = variants[0]
    pdb = args.pdb / "posterior_database"
    refs, _ = verify_refs.load_refs()
    cases = corpus_cases(pdb, include_language=True)
    shard, nshards = (int(x) for x in args.shard.split("/"))
    models = args.models or sorted(refs)
    models = models[shard::nshards]
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="bench_isa_"))
    start = time.time()

    cols = (["model", "collection", "n_values", "status"]
            + [f"{n}_ns" for n, _ in variants]
            + [f"{n}_speedup" for n, _ in variants[1:]]
            + [f"{n}_ulp" for n, _ in variants[1:]])
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w") as out:
        out.write("\t".join(cols) + "\n")
        for idx, model in enumerate(models):
            row = {"model": model,
                   "collection": getattr(cases.get(model), "collection", "?"),
                   "n_values": "", "status": "ok"}
            elapsed = (time.time() - start) / 60
            if elapsed > args.budget_min:
                row["status"] = "skipped_budget"
            else:
                try:
                    stan, data = verify_refs.model_files(
                        model, refs[model], pdb, tmp)
                    if not stan.exists() or not data.exists():
                        row["status"] = "missing_input"
                    else:
                        measure(row, model, stan, data, variants, base_dir,
                                tmp, args)
                except Exception as exc:
                    row["status"] = f"error {type(exc).__name__}: {exc}"[:120]
            out.write("\t".join(str(row.get(c, "")) for c in cols) + "\n")
            out.flush()
            print(f"[{idx + 1}/{len(models)}] {elapsed:6.1f} min {model} "
                  f"{row['status']} "
                  + " ".join(f"{row.get(f'{n}_speedup', '')}"
                             for n, _ in variants[1:]), flush=True)


def measure(row, model, stan, data, variants, base_dir, tmp, args):
    dump = tmp / f"mir_{model}"
    dump.mkdir(parents=True, exist_ok=True)
    try:
        subprocess.run(
            [str(base_dir / "stanli_check"), str(stan), str(data),
             "--dump-passes=mir", f"--dump-dir={dump}"],
            capture_output=True, text=True, timeout=args.timeout, cwd=REPO)
    except subprocess.TimeoutExpired:
        row["status"] = "dump_timeout"
        return
    mir = dump / "00-mir.sexp"
    if not mir.exists():
        row["status"] = "no_mir"
        return
    samples = {name: [] for name, _ in variants}
    values = {}
    names = [n for n, _ in variants]
    for r in range(args.rounds):
        order = names[r % len(names):] + names[:r % len(names)]
        for name in order:
            binary = dict(variants)[name] / "bench_grad"
            result, err = run_bench(binary, mir, data, args.warmup_ms,
                                    args.measure_ms, args.timeout)
            if result is None:
                row["status"] = f"{name}: {err}"[:120]
                return
            samples[name].append(result["elapsed_ns"] / result["iterations"])
            values.setdefault(name, result["values"])
    row["n_values"] = len(values[names[0]])
    med = {n: statistics.median(samples[n]) for n in names}
    for n in names:
        row[f"{n}_ns"] = f"{med[n]:.1f}"
    for n in names[1:]:
        row[f"{n}_speedup"] = f"{med[names[0]] / med[n]:.4f}"
        row[f"{n}_ulp"] = max_ulp(values[names[0]], values[n])
    shutil.rmtree(dump, ignore_errors=True)


if __name__ == "__main__":
    main()
