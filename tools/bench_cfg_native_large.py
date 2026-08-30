#!/usr/bin/env python3
"""Alternate production native and var replay on the large CFG canary.

This is a manual performance gate, not a CI timing test. Correctness and the
structural census live in test_cfg_native_large; this script deliberately
reports an honest positive or negative timing result without choosing a
threshold from the result.

From the repository root:

    ./build-audit/test_cfg_native_large
    python3 tools/bench_cfg_native_large.py --build-dir build-audit
    python3 tools/bench_cfg_native_large.py --build-dir build-audit --theta -0.1
"""

from __future__ import annotations

import argparse
import math
import os
import pathlib
import statistics
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
MIR = ROOT / "tests/fixtures/cfg_native_large_structured.tmir.sexp"
DATA = ROOT / "tests/fixtures/cfg_native_large_structured.json"


def one(
    binary: pathlib.Path, evaluations: int, production_native: bool, theta: float
) -> tuple[float, float, float, int]:
    environment = os.environ.copy()
    # Benchmark the shipped production decision, including its dimension-
    # gated structured pullbacks. Never let an ambient diagnostic force,
    # threshold, escape hatch, or global native override change either arm.
    for flag in (
        "STANLI_CFG_STRUCTURED_NATIVE",
        "STANLI_CFG_PREPARED_MDIVIDE_LEFT",
        "STANLI_CFG_PREPARED_MDIVIDE_LEFT_MIN_N",
        "STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU",
        "STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU_MIN_N",
        "STANLI_NO_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU",
        "STANLI_CFG_MATRIX_EXP_BLOCK_FRECHET",
        "STANLI_CFG_MATRIX_EXP_BLOCK_FRECHET_MIN_N",
        "STANLI_NO_CFG_MATRIX_EXP_BLOCK_FRECHET",
    ):
        environment.pop(flag, None)
    if production_native:
        environment.pop("STANLI_NO_NATIVE_ADJ", None)
    else:
        environment["STANLI_NO_NATIVE_ADJ"] = "1"
    completed = subprocess.run(
        [
            str(binary),
            str(MIR),
            str(DATA),
            str(evaluations),
            "--set-param",
            "0",
            format(theta, ".17g"),
        ],
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
    gradient, sink, forward, parameters = (
        float(fields[0]),
        float(fields[1]),
        float(fields[2]),
        int(fields[3]),
    )
    if not all(math.isfinite(value) for value in (gradient, sink, forward)):
        raise RuntimeError(f"nonfinite benchmark result: {completed.stdout!r}")
    return gradient, sink, forward, parameters


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build-audit")
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--evals", type=int, default=50)
    parser.add_argument(
        "--theta",
        type=float,
        default=0.1,
        help="unconstrained theta; positive selects structured, nonpositive fallback",
    )
    parser.add_argument(
        "--min-speedup",
        type=float,
        default=0.0,
        help="optional manual gate; zero reports without enforcing a result",
    )
    arguments = parser.parse_args()
    if arguments.samples < 1 or arguments.evals < 1:
        parser.error("--samples and --evals must be positive")
    if not math.isfinite(arguments.theta):
        parser.error("--theta must be finite")

    binary = (ROOT / arguments.build_dir / "bench_grad").resolve()
    for required in (binary, MIR, DATA):
        if not required.is_file():
            parser.error(f"required file not found: {required}")

    native: list[float] = []
    replay: list[float] = []
    native_forward: list[float] = []
    replay_forward: list[float] = []
    expected_params: int | None = None
    sinks: dict[str, list[float]] = {"native": [], "replay": []}
    for sample in range(arguments.samples):
        # Alternate the leading arm so neither mode always receives the
        # colder process or the hotter half of the frequency curve.
        modes = (True, False) if sample % 2 == 0 else (False, True)
        for production_native in modes:
            gradient, sink, forward, parameters = one(
                binary, arguments.evals, production_native, arguments.theta
            )
            if expected_params is None:
                expected_params = parameters
            if parameters != expected_params:
                raise RuntimeError(
                    f"parameter-count mismatch: {parameters}/{expected_params}"
                )
            label = "production-native" if production_native else "var-replay"
            print(
                f"  sample={sample + 1} arm={label} "
                f"gradient={gradient:.1f} ns forward={forward:.1f} ns "
                f"sink={sink:.9g}"
            )
            sink_key = "native" if production_native else "replay"
            sinks[sink_key].append(sink)
            (native if production_native else replay).append(gradient)
            (native_forward if production_native else replay_forward).append(
                forward
            )

    native_median = statistics.median(native)
    replay_median = statistics.median(replay)
    speedup = replay_median / native_median
    print(
        f"cfg_native_large samples={arguments.samples} evals={arguments.evals} "
        f"params={expected_params} theta={arguments.theta:.17g}"
    )
    print(
        f"  production native gradient median={native_median:.1f} ns "
        f"forward={statistics.median(native_forward):.1f} ns"
    )
    print(
        f"  var replay gradient median={replay_median:.1f} ns "
        f"forward={statistics.median(replay_forward):.1f} ns"
    )
    print(
        f"  speedup={speedup:.3f}x "
        f"sink_native={statistics.median(sinks['native']):.9g} "
        f"sink_replay={statistics.median(sinks['replay']):.9g}"
    )
    # bench_grad includes a time-bounded warmup in its sink, so the two arms
    # may perform different warmup counts. test_cfg_native_large, not this
    # diagnostic sink, is the semantic oracle.
    if arguments.min_speedup > 0 and speedup < arguments.min_speedup:
        print(
            f"speedup {speedup:.3f}x is below {arguments.min_speedup:.3f}x",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
