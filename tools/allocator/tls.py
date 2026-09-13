#!/usr/bin/env python3
"""Frozen Linux TLS-backend discriminator; no shipping allocator policy change."""
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
    variants = {name: dict(library=str((args.libraries / directory /
                    "libstanli_allocator_benchmark.so").resolve()),
                    mode="system" if name == "system" else "private", placement="worker",
                    environment={"STANLI_ALLOCATOR_BENCH_WARMUP_MS": "0"})
                for name, directory in [("system", "SYSTEM"), ("dynamic", "MIMALLOC"),
                                         ("pthread", "PTHREADS")]}
    design = dict(baseline="system", variants=variants,
        cells=[["normal_8", w] for w in (1, 2, 4)] + [["normal_1024", 4], ["normal_262144", 4]],
        comparisons=[["dynamic/system", "system", "dynamic"],
                     ["pthread/system", "system", "pthread"],
                     ["pthread/dynamic", "dynamic", "pthread"]])
    args.design = args.output.with_suffix(".design.json")
    assert not args.design.exists()
    save(args.design, design)
    args.system = args.candidate = None
    args.verify_only = False
    experiment(args)
