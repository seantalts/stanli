#!/usr/bin/env python3
"""Frozen sustained-native-warmup check after the host-startup discriminator."""
import argparse
import pathlib

from bench import save
from placement import experiment

if __name__ == "__main__":
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--inputs", type=pathlib.Path, required=True)
    parser.add_argument("--libraries", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    variants = {}
    for setting, milliseconds in (("short", "0"), ("warm", "500")):
        for mode, directory in (("system", "SYSTEM"), ("private", "MIMALLOC")):
            variants[mode + "-" + setting] = dict(
                library=str((args.libraries / directory / "libstanli_allocator_benchmark.so").resolve()),
                mode=mode, placement="worker", host_diagnostics=True,
                unset_environment=["OPENBLAS_NUM_THREADS", "OPENBLAS_DEFAULT_NUM_THREADS", "GOTO_NUM_THREADS", "OMP_NUM_THREADS"],
                environment={"STANLI_ALLOCATOR_BENCH_WARMUP_MS": milliseconds})
    repetitions = [("normal_8", 1, 500000), ("normal_8", 4, 100000),
                   ("eight_schools_noncentered", 4, 100000),
                   ("normal_1024", 4, 30000), ("normal_262144", 4, 128)]
    design = dict(baseline="system-short", variants=variants,
        cells=[[name, workers] for name, workers, _ in repetitions],
        fixed_repetitions=[dict(cell=[name, workers], reps=reps) for name, workers, reps in repetitions],
        comparisons=[[setting, "system-" + setting, "private-" + setting]
                     for setting in ("short", "warm")] +
                    [[mode + "-warm/short", mode + "-short", mode + "-warm"]
                     for mode in ("system", "private")])
    args.design = args.output.with_suffix(".design.json")
    assert not args.design.exists()
    save(args.design, design)
    args.system = args.candidate = None
    args.verify_only = False
    experiment(args)
