#!/usr/bin/env python3
"""Paired gradient timings of two or more bench_grad binaries over the corpus.

python3 harnesses/ab_bench_corpus.py PDB OUT_DIR --arm base=BIN --arm new=BIN \
    [--arm aa=BIN_COPY] [--rounds 7] [--filter SUBSTR] [--limit N]
    [--fast-math] [--arm-env new:STANLI_NO_COLLAPSE=1]

Uses the protocol of harnesses/corpus_bench.py (docs/benchmarks.md#how-we-measure):
MIR from the vectorizing stanc, bench_grad --timed with a 200 ms warmup and a
250 ms window, one fresh process per sample, one thread requested, one
process at a time, the first arm rotating each round. Nothing here compares
against CmdStan. OUT_DIR/results.jsonl holds one record per model (every
sample, the order, the load average, the density and gradient values);
rerunning with the same OUT_DIR skips models already recorded.
Binaries should be copies, so that no rebuild can change them mid-sweep.
--fast-math builds the MIR in fast mode and passes --fast-math to every arm;
--arm-env sets one environment variable for one arm, so a single binary can
be compared with a pass switched off.
"""
import argparse
import hashlib
import json
import os
import pathlib
import sys
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "harnesses"))
from corpus_bench import (PhaseFailure, Runner, VECTORIZE_PROBE,  # noqa: E402
                          benchmark_cases, materialize_data, parse_timing)


def rotation(arms, round_index):
    k = round_index % len(arms)
    return arms[k:] + arms[:k]


def wait_for_quiet(limit, log, minutes=30):
    deadline = time.time() + minutes * 60
    while os.getloadavg()[0] > limit and time.time() < deadline:
        log.append(dict(paused_at=time.time(), load=os.getloadavg()[0]))
        time.sleep(30)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("pdb", type=pathlib.Path)
    ap.add_argument("output", type=pathlib.Path)
    ap.add_argument("--arm", action="append", required=True)
    ap.add_argument("--rounds", type=int, default=7)
    ap.add_argument("--warmup-ms", type=int, default=200)
    ap.add_argument("--measure-ms", type=int, default=250)
    ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("--filter", default="")
    ap.add_argument("--models", default="",
                    help="file with one model name per line")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--load-limit", type=float, default=3.0)
    ap.add_argument("--fast-math", action="store_true")
    ap.add_argument("--arm-env", action="append", default=[],
                    metavar="ARM:VAR=VALUE")
    args = ap.parse_args()
    arms = [a.split("=", 1) for a in args.arm]
    names = [n for n, _ in arms]
    binaries = {n: pathlib.Path(b).resolve() for n, b in arms}
    arm_env = {n: [] for n in names}
    for setting in args.arm_env:
        arm, assignment = setting.split(":", 1)
        arm_env[arm].append(assignment)
    fast = ["--fast-math"] if args.fast_math else []
    args.output.mkdir(parents=True, exist_ok=True)
    results = args.output / "results.jsonl"
    done = set()
    if results.exists():
        done = {json.loads(line)["model"] for line in results.open()}
    cases = benchmark_cases(args.pdb, "all")
    wanted = set(pathlib.Path(args.models).read_text().split()) if args.models else None
    selected = [n for n in sorted(cases)
                if args.filter in n and (wanted is None or n in wanted)]
    if args.limit:
        selected = selected[:args.limit]
    runner = Runner(args.output)
    work = args.output / "work"
    work.mkdir(exist_ok=True)
    (args.output / "manifest.json").write_text(json.dumps(dict(
        arms={n: dict(path=str(b), sha256=hashlib.sha256(b.read_bytes()).hexdigest())
              for n, b in binaries.items()},
        fast_math=args.fast_math, arm_env=arm_env,
        rounds=args.rounds, warmup_ms=args.warmup_ms, measure_ms=args.measure_ms,
        thread_env="STAN_NUM_THREADS=1", started=time.ctime()), indent=1))
    pauses = []
    for index, name in enumerate(selected):
        if name in done:
            continue
        stan, data = cases[name]
        record = dict(model=name, status="failed", note="", samples={n: [] for n in names},
                      orders=[], values={}, load_before=os.getloadavg())
        directory = work / name
        directory.mkdir(exist_ok=True)
        try:
            wait_for_quiet(args.load_limit, pauses)
            record["load_before"] = os.getloadavg()
            source, mir, data_json = (directory / "model.stan", directory / "model.sexp",
                                      directory / "data.json")
            source.write_bytes(stan.read_bytes())
            materialize_data(data, data_json)
            runner.require(f"{name}/mir", [VECTORIZE_PROBE, "--vectorize-loops", "on",
                                           *fast, "--output", mir, source], args.timeout)
            for round_index in range(args.rounds):
                order = rotation(names, round_index)
                record["orders"].append(order)
                for arm in order:
                    event = runner.require(
                        f"{name}/gradient/{round_index}/{arm}",
                        [*(["env", *arm_env[arm]] if arm_env[arm] else []),
                         binaries[arm], mir, data_json, "--timed", *fast, "--warmup-ms",
                         str(args.warmup_ms), "--measure-ms", str(args.measure_ms)],
                        args.timeout)
                    timing = parse_timing(runner.text(event), args.warmup_ms, args.measure_ms)
                    record["samples"][arm].append(timing["elapsed_ns"] / timing["iterations"])
                    record["values"].setdefault(arm, timing["values"])
            record["status"] = "ok"
        except (PhaseFailure, ValueError) as exc:
            record["note"] = str(exc)
        record["load_after"] = os.getloadavg()
        with results.open("a") as out:
            out.write(json.dumps(record) + "\n")
        print(f"[{index + 1}/{len(selected)}] {name} {record['status']} {record['note'][:60]}",
              flush=True)
    if pauses:
        (args.output / "pauses.json").write_text(json.dumps(pauses))


if __name__ == "__main__":
    main()
