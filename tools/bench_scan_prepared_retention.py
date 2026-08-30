#!/usr/bin/env python3
"""A/B retained prim-LU factors inside an unrelated scheduled scan.

The fixture has 513 rows: one peeled row followed by an alternating pair of
cheap subject-start and continuation templates.  The continuation template's
parameter-taken arm contains one active 55x55 solve; the untaken arm and the
subject-start template contain no solve.

This manual benchmark holds native structured CFG and prepared prim-LU
generation fixed, then compares scan-level retention with its authoritative
escape hatch. It alternates which process runs first, checks the complete
``stanli_check`` output before timing, and reports both gradient and
forward-only medians.  ``--forward-only`` selects bench_grad's forward-only
field as the win/gate metric; both fields are always printed.

From the repository root:

    python3 tools/bench_scan_prepared_retention.py --build-dir build-audit
    python3 tools/bench_scan_prepared_retention.py \
        --build-dir build-audit --arm untaken --forward-only
"""

from __future__ import annotations

import argparse
import math
import os
import pathlib
import statistics
import struct
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
STAN = ROOT / "tests/fixtures/scan_prepared_solve_retention.stan"
MIR = ROOT / "tests/fixtures/scan_prepared_solve_retention.tmir.sexp"
DATA = ROOT / "tests/fixtures/scan_prepared_solve_retention.json"

RETENTION_FORCE = "STANLI_SCAN_PREPARED_RETENTION"
RETENTION_ESCAPE = "STANLI_NO_SCAN_PREPARED_RETENTION"
SCRUB = (
    "STANLI_PROFILE",
    "STANLI_PROFILE_PREP",
    "STANLI_PROFILE_SCAN",
    "STANLI_DEBUG_SCAN",
    "STANLI_NO_SCAN",
    "STANLI_NO_SCAN_INVARIANT_CACHE",
    "STANLI_NO_SCAN_SPARSE_ADJ_RESET",
    "STANLI_NO_NATIVE_ADJ",
    "STANLI_CFG_STRUCTURED_NATIVE",
    "STANLI_CFG_PREPARED_MDIVIDE_LEFT",
    "STANLI_CFG_PREPARED_MDIVIDE_LEFT_MIN_N",
    "STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU",
    "STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU_MIN_N",
    "STANLI_NO_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU",
    RETENTION_FORCE,
    RETENTION_ESCAPE,
)


def environment(retained: bool) -> dict[str, str]:
    result = os.environ.copy()
    for flag in SCRUB:
        result.pop(flag, None)
    # Both arms select exactly the same native structured program and prepared
    # prim-LU CALL. Only the scan-level retention plan differs.
    result["STANLI_CFG_STRUCTURED_NATIVE"] = "1"
    result["STANLI_CFG_PREPARED_MDIVIDE_LEFT_PRIM_LU"] = "1"
    if retained:
        result[RETENTION_FORCE] = "1"
    else:
        result[RETENTION_ESCAPE] = "1"
    return result


def invoke(argv: list[str], *, retained: bool) -> str:
    completed = subprocess.run(
        argv,
        cwd=ROOT,
        env=environment(retained),
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed ({completed.returncode}): {' '.join(argv)}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed.stdout


def ordered_double(value: float) -> int:
    bits = struct.unpack(">q", struct.pack(">d", value))[0]
    return 0x8000000000000000 - bits if bits < 0 else bits


def compare_check_outputs(left: str, right: str) -> tuple[int, int, float, int]:
    lhs = left.strip().split()
    rhs = right.strip().split()
    if not lhs or lhs[0] != "OK" or not rhs or rhs[0] != "OK":
        raise RuntimeError("stanli_check did not produce an OK result")
    if len(lhs) != len(rhs):
        raise RuntimeError(
            f"stanli_check result lengths differ: {len(lhs)} != {len(rhs)}"
        )

    exact = 0
    max_relative = 0.0
    max_ulp = 0
    for a_text, b_text in zip(lhs[1:], rhs[1:]):
        a, b = float(a_text), float(b_text)
        if math.isnan(a) or math.isnan(b):
            if not (math.isnan(a) and math.isnan(b)):
                return exact, len(lhs) - 1, math.inf, 2**64 - 1
            exact += 1
            continue
        if struct.pack(">d", a) == struct.pack(">d", b):
            exact += 1
            continue
        if not (math.isfinite(a) and math.isfinite(b)):
            return exact, len(lhs) - 1, math.inf, 2**64 - 1
        scale = max(abs(a), abs(b), sys.float_info.min)
        max_relative = max(max_relative, abs(a - b) / scale)
        max_ulp = max(max_ulp, abs(ordered_double(a) - ordered_double(b)))
    return exact, len(lhs) - 1, max_relative, max_ulp


def correctness_check(
    check_binary: pathlib.Path,
    arm: str,
    max_relative: float,
    require_bitwise: bool,
) -> None:
    # stanli_check point zero has theta=+0.1; point one has theta=-0.04.
    point = 0 if arm == "taken" else 1
    command = [
        str(check_binary),
        str(STAN),
        str(DATA),
        "--mir",
        str(MIR),
        "--point",
        str(point),
        "--lp-only",
    ]
    retained = invoke(command, retained=True)
    unretained = invoke(command, retained=False)
    exact, total, relative, ulp = compare_check_outputs(retained, unretained)

    retained_values = retained.strip().split()
    unretained_values = unretained.strip().split()
    lp_exact = (
        len(retained_values) > 1
        and len(unretained_values) > 1
        and struct.pack(">d", float(retained_values[1]))
        == struct.pack(">d", float(unretained_values[1]))
    )
    print(
        f"  exact-output arm={arm} point={point} lp_exact={int(lp_exact)} "
        f"exact={exact}/{total} max_rel={relative:.3e} max_ulp={ulp}"
    )
    if not lp_exact:
        raise RuntimeError(f"{arm}: retained factors changed the log density")
    if relative > max_relative:
        raise RuntimeError(
            f"{arm}: retained/unretained outputs differ by {relative:.3e} "
            f"relative (allowed {max_relative:.3e})"
        )
    if require_bitwise and exact != total:
        raise RuntimeError(
            f"{arm}: bitwise output required, but only {exact}/{total} values match"
        )


def benchmark_once(
    binary: pathlib.Path,
    evaluations: int,
    retained: bool,
    theta: float,
) -> tuple[float, float, float, int]:
    output = invoke(
        [
            str(binary),
            str(MIR),
            str(DATA),
            str(evaluations),
            "--set-param",
            "0",
            format(theta, ".17g"),
        ],
        retained=retained,
    )
    fields = output.strip().splitlines()[-1].split()
    if len(fields) != 4:
        raise RuntimeError(f"unexpected bench_grad output: {output!r}")
    gradient, sink, forward, parameters = (
        float(fields[0]),
        float(fields[1]),
        float(fields[2]),
        int(fields[3]),
    )
    if not all(math.isfinite(value) for value in (gradient, sink, forward)):
        raise RuntimeError(f"nonfinite benchmark result: {output!r}")
    return gradient, sink, forward, parameters


def benchmark_arm(
    binary: pathlib.Path,
    arm: str,
    theta: float,
    samples: int,
    evaluations: int,
    forward_only: bool,
    min_speedup: float,
) -> bool:
    gradients: dict[bool, list[float]] = {True: [], False: []}
    forwards: dict[bool, list[float]] = {True: [], False: []}
    expected_parameters: int | None = None

    for sample in range(samples):
        # Alternate the leading mode so neither always receives the colder
        # process or the hotter half of the machine's frequency curve.
        modes = (True, False) if sample % 2 == 0 else (False, True)
        for retained in modes:
            gradient, sink, forward, parameters = benchmark_once(
                binary, evaluations, retained, theta
            )
            if expected_parameters is None:
                expected_parameters = parameters
            if parameters != expected_parameters:
                raise RuntimeError(
                    f"parameter-count mismatch: {parameters}/{expected_parameters}"
                )
            gradients[retained].append(gradient)
            forwards[retained].append(forward)
            label = "retained" if retained else "unretained"
            print(
                f"  sample={sample + 1} arm={arm} mode={label} "
                f"gradient={gradient:.1f} ns forward={forward:.1f} ns "
                f"sink={sink:.9g}"
            )

    retained_gradient = statistics.median(gradients[True])
    unretained_gradient = statistics.median(gradients[False])
    retained_forward = statistics.median(forwards[True])
    unretained_forward = statistics.median(forwards[False])
    gradient_speedup = unretained_gradient / retained_gradient
    forward_speedup = unretained_forward / retained_forward
    gradient_wins = sum(
        retained < unretained
        for retained, unretained in zip(gradients[True], gradients[False])
    )
    forward_wins = sum(
        retained < unretained
        for retained, unretained in zip(forwards[True], forwards[False])
    )
    selected_speedup = forward_speedup if forward_only else gradient_speedup
    selected_wins = forward_wins if forward_only else gradient_wins
    metric = "forward-only" if forward_only else "gradient"

    print(
        f"scan_prepared_retention arm={arm} theta={theta:.17g} "
        f"samples={samples} evals={evaluations} params={expected_parameters}"
    )
    print(
        f"  gradient retained={retained_gradient:.1f} ns "
        f"unretained={unretained_gradient:.1f} ns "
        f"speedup={gradient_speedup:.3f}x wins={gradient_wins}/{samples}"
    )
    print(
        f"  forward-only retained={retained_forward:.1f} ns "
        f"unretained={unretained_forward:.1f} ns "
        f"speedup={forward_speedup:.3f}x wins={forward_wins}/{samples}"
    )
    print(
        f"  selected metric={metric} speedup={selected_speedup:.3f}x "
        f"wins={selected_wins}/{samples}"
    )
    if min_speedup > 0 and selected_speedup < min_speedup:
        print(
            f"{arm} {metric} speedup {selected_speedup:.3f}x is below "
            f"{min_speedup:.3f}x",
            file=sys.stderr,
        )
        return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build-audit")
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--evals", type=int, default=3)
    parser.add_argument(
        "--arm", choices=("taken", "untaken", "both"), default="both"
    )
    parser.add_argument("--taken-theta", type=float, default=0.1)
    parser.add_argument("--untaken-theta", type=float, default=-0.1)
    parser.add_argument(
        "--forward-only",
        action="store_true",
        help="select bench_grad's forward-only field for wins and gating",
    )
    parser.add_argument(
        "--min-speedup",
        type=float,
        default=0.0,
        help="optional selected-metric gate; zero only reports",
    )
    parser.add_argument(
        "--max-relative",
        type=float,
        default=1e-10,
        help="semantic ceiling after requiring a bitwise-identical LP",
    )
    parser.add_argument(
        "--require-bitwise",
        action="store_true",
        help="require every stanli_check LP/gradient value to match bitwise",
    )
    arguments = parser.parse_args()

    if arguments.samples < 1 or arguments.evals < 1:
        parser.error("--samples and --evals must be positive")
    if arguments.min_speedup < 0:
        parser.error("--min-speedup must be nonnegative")
    if not math.isfinite(arguments.max_relative) or arguments.max_relative < 0:
        parser.error("--max-relative must be finite and nonnegative")
    for name, value in (
        ("--taken-theta", arguments.taken_theta),
        ("--untaken-theta", arguments.untaken_theta),
    ):
        if not math.isfinite(value):
            parser.error(f"{name} must be finite")
    if arguments.taken_theta <= 0:
        parser.error("--taken-theta must be positive")
    if arguments.untaken_theta > 0:
        parser.error("--untaken-theta must be nonpositive")

    build = (ROOT / arguments.build_dir).resolve()
    bench = build / "bench_grad"
    check = build / "stanli_check"
    for label, path in (
        ("Stan fixture", STAN),
        ("MIR fixture", MIR),
        ("data fixture", DATA),
        ("bench_grad", bench),
        ("stanli_check", check),
    ):
        if not path.is_file():
            parser.error(f"{label} not found: {path}")

    arms = ("taken", "untaken") if arguments.arm == "both" else (arguments.arm,)
    for arm in arms:
        correctness_check(
            check, arm, arguments.max_relative, arguments.require_bitwise
        )

    ok = True
    for arm in arms:
        theta = (
            arguments.taken_theta if arm == "taken" else arguments.untaken_theta
        )
        ok = (
            benchmark_arm(
                bench,
                arm,
                theta,
                arguments.samples,
                arguments.evals,
                arguments.forward_only,
                arguments.min_speedup,
            )
            and ok
        )
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"bench_scan_prepared_retention: {error}", file=sys.stderr)
        raise SystemExit(1)
