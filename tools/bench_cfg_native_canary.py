#!/usr/bin/env python3
"""Time the durable, non-ctsem forward-CFG native-adjoint canary.

The structural and bit-exact gate is `test_cfg_native_canary`; this script is
the intentionally manual performance gate. It times pr236_island, a scalar
CFG that the production profitability policy selects (not the tiny structured
diag case that policy intentionally rejects). It alternates native and replay
processes, checks that their machine-readable sinks agree, and reports medians.

From the repository root after building `bench_grad` and the canary test:

    ./build-audit/test_cfg_native_canary
    python3 tools/bench_cfg_native_canary.py --build-dir build-audit

Use `--min-speedup 0` for measurement-only runs. The default 1.25 threshold
is deliberately below the roughly 1.9x observed on Apple M-series machines;
it catches loss of the optimization without treating this as a CI benchmark.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import statistics
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
MIR = ROOT / "tests/fixtures/pr236_island.tmir.sexp"
DATA = ROOT / "tests/fixtures/paramcond.json"


def one(binary: pathlib.Path, evaluations: int, replay: bool) -> tuple[float, str, float, int]:
    environment = os.environ.copy()
    if replay:
        environment["STANLI_NO_NATIVE_ADJ"] = "1"
    else:
        environment.pop("STANLI_NO_NATIVE_ADJ", None)
    completed = subprocess.run(
        [str(binary), str(MIR), str(DATA), str(evaluations)],
        cwd=ROOT,
        env=environment,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    fields = completed.stdout.split()
    if len(fields) != 4:
        raise RuntimeError(f"unexpected bench_grad output: {completed.stdout!r}")
    return float(fields[0]), fields[1], float(fields[2]), int(fields[3])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build-audit")
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--evals", type=int, default=200_000)
    parser.add_argument("--min-speedup", type=float, default=1.25)
    arguments = parser.parse_args()
    if arguments.samples < 1 or arguments.evals < 1:
        parser.error("--samples and --evals must be positive")

    binary = (ROOT / arguments.build_dir / "bench_grad").resolve()
    if not binary.is_file():
        parser.error(f"bench_grad not found: {binary}")

    native: list[float] = []
    replay: list[float] = []
    native_forward: list[float] = []
    replay_forward: list[float] = []
    expected_sink: str | None = None
    expected_params: int | None = None
    for sample in range(arguments.samples):
        # Alternating the leading mode prevents one mode from always receiving
        # the colder or hotter half of a laptop's frequency curve.
        modes = (False, True) if sample % 2 == 0 else (True, False)
        for force_replay in modes:
            elapsed, sink, forward, parameters = one(
                binary, arguments.evals, force_replay
            )
            if expected_sink is None:
                expected_sink, expected_params = sink, parameters
            if sink != expected_sink or parameters != expected_params:
                raise RuntimeError(
                    "native/replay semantic mismatch: "
                    f"sink={sink}/{expected_sink}, params={parameters}/{expected_params}"
                )
            (replay if force_replay else native).append(elapsed)
            (replay_forward if force_replay else native_forward).append(forward)

    native_median = statistics.median(native)
    replay_median = statistics.median(replay)
    speedup = replay_median / native_median
    print(
        f"cfg_native_canary samples={arguments.samples} evals={arguments.evals} "
        f"params={expected_params} sink={expected_sink}"
    )
    print(
        f"  native gradient median={native_median:.1f} ns "
        f"forward={statistics.median(native_forward):.1f} ns"
    )
    print(
        f"  replay gradient median={replay_median:.1f} ns "
        f"forward={statistics.median(replay_forward):.1f} ns"
    )
    print(f"  speedup={speedup:.3f}x")
    if arguments.min_speedup > 0 and speedup < arguments.min_speedup:
        print(
            f"speedup {speedup:.3f}x is below {arguments.min_speedup:.3f}x",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
