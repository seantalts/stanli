#!/usr/bin/env python3
"""Matched existing bench_grad CLI consumers: SYSTEM/OFF vs SYSTEM/aligned.

Twenty processes per model in five balanced rounds, including both A/A aliases.
The published CLI's fixed point and 200 ms warmup remain completely unchanged.
Baseline-only calibration fixes about 150 ms of measured gradients per process.
"""
import argparse
import hashlib
import json
import math
import os
import pathlib
import random
import statistics as stats
import subprocess
import time


def main(args):
    out = args.output.resolve()
    out.mkdir(parents=True)
    (out / "logs").mkdir()
    cases = json.loads(args.inputs.read_text())["cases"]
    slots = ["system", "aligned", "system_aa", "aligned_aa"]
    binaries = dict(system=str(args.system.resolve()), aligned=str(args.aligned.resolve()))
    env = {k: v for k, v in os.environ.items() if not k.startswith(
        ("DYLD_", "STANLI_", "MIMALLOC_", "TCMALLOC_", "Malloc"))}
    manifest = dict(binaries={k: dict(path=v, sha256=hashlib.sha256(pathlib.Path(v).read_bytes()).hexdigest())
                              for k, v in binaries.items()}, rounds=5, cases=cases, started=time.time())
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2))
    index = (out / "runs.jsonl").open("x")
    rows = []

    def one(case, slot, reps, rnd):
        inp = pathlib.Path(case["input_dir"])
        command = [binaries[slot.removesuffix("_aa")], str(inp / "model.mir"), str(inp / "data.json"), str(reps)]
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=180)
        label = f"{len(rows):04d}-{case['name']}-{slot}"
        (out / "logs" / (label + ".stdout")).write_text(result.stdout)
        (out / "logs" / (label + ".stderr")).write_text(result.stderr)
        assert result.returncode == 0, result.stderr
        ns, sink, forward, params = map(float, result.stdout.split())
        assert all(math.isfinite(x) for x in (ns, sink, forward, params))
        row = dict(name=case["name"], slot=slot, round=rnd, reps=reps, ns=ns,
                   forward_ns=forward, params=params, command=command)
        rows.append(row)
        index.write(json.dumps(row) + "\n")
        index.flush()
        return row

    calibration = {}
    for case in cases:
        r = one(case, "system", 1024, -1)
        calibration[case["name"]] = max(64, min(4000000, math.ceil(150e6 / r["ns"])))
    (out / "calibration.json").write_text(json.dumps(calibration, indent=2))
    for rnd in range(5):
        shuffled = cases[:]
        random.Random(49145 + rnd).shuffle(shuffled)
        order = slots[rnd % 4:] + slots[:rnd % 4]
        if rnd % 2:
            order = order[::-1]
        for case in shuffled:
            for slot in order:
                one(case, slot, calibration[case["name"]], rnd)
        print("CLI round", rnd + 1, "complete", flush=True)
    index.close()
    summary = []
    for case in cases:
        group = [r for r in rows if r["name"] == case["name"] and r["round"] >= 0]
        assert len({r["params"] for r in group}) == 1
        values = {slot: [r["ns"] for r in group if r["slot"] == slot] for slot in slots}
        ratios = {}
        for key, a, b in [("aligned", "system", "aligned"), ("aa_system", "system", "system_aa"),
                          ("aa_aligned", "aligned", "aligned_aa")]:
            v = [x / y for x, y in zip(values[a], values[b])]
            ratios[key] = dict(median=stats.median(v), minimum=min(v), maximum=max(v),
                               rounds=v, above_one=sum(x > 1 for x in v))
        summary.append(dict(name=case["name"], median_ns={k:stats.median(v) for k,v in values.items()}, ratios=ratios))
    (out / "summary.json").write_text(json.dumps(dict(cells=summary, processes=len(rows)), indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--system", type=pathlib.Path, required=True)
    parser.add_argument("--aligned", type=pathlib.Path, required=True)
    parser.add_argument("--inputs", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    main(parser.parse_args())
