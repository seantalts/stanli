#!/usr/bin/env python3
"""Scale the issue-248 ctsem data and A/B OP_SCAN against unrolling.

The ctsem model and its data are deliberately not stored in this repository.
Pass the transformed (O0) MIR and the complete 4,000-row issue attachment.
This harness writes exact prefix datasets, checks the log density and all
gradients with ``stanli_check``, inventories the lowered graph, and records
preparation and one-gradient timings for scan-on and ``STANLI_NO_SCAN=1``.

The benchmark's ``sink`` is not a correctness oracle: bench_grad includes a
time-bounded warmup in it.  Semantic comparison therefore always comes from
stanli_check's deterministic point.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import re
import statistics
import struct
import subprocess
import sys
import tempfile
import time


ROW_FIELDS = (
    "Y",
    "subject",
    "time",
    "nobs_y",
    "whichobs_y",
    "nbinary_y",
    "whichbinary_y",
    "ncont_y",
    "whichcont_y",
    "tdpreds",
    "dokalmanrows",
)
# N=32 is the exact scheduled-scan threshold for ctsem's normalized loop;
# N=33 is the first case above it. Keep both to catch inclusive-bound errors.
SIZES = (1, 2, 8, 16, 32, 33)
GRAPH_HEADER = re.compile(r"^slots=(\d+) ops=(\d+) result=(-?\d+)$")
SLOT_ELEMS = re.compile(
    r"stanli_prep graph=log_prob stage=total .*\bslot_elems=(\d+)\b"
)


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as src:
        for block in iter(lambda: src.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def environment(scan: bool) -> dict[str, str]:
    env = os.environ.copy()
    if scan:
        env.pop("STANLI_NO_SCAN", None)
    else:
        env["STANLI_NO_SCAN"] = "1"
    return env


def run(
    argv: list[str],
    *,
    env: dict[str, str],
    stdout_path: pathlib.Path,
    stderr_path: pathlib.Path,
) -> tuple[str, str, float]:
    started = time.perf_counter()
    proc = subprocess.run(argv, env=env, text=True, capture_output=True)
    elapsed = time.perf_counter() - started
    stdout_path.write_text(proc.stdout)
    stderr_path.write_text(proc.stderr)
    if proc.returncode:
        command = " ".join(argv)
        raise RuntimeError(
            f"command failed ({proc.returncode}): {command}\n"
            f"stdout: {stdout_path}\nstderr: {stderr_path}"
        )
    return proc.stdout, proc.stderr, elapsed


def ordered_double(value: float) -> int:
    bits = struct.unpack(">q", struct.pack(">d", value))[0]
    return 0x8000000000000000 - bits if bits < 0 else bits


def compare_ok_lines(left: str, right: str) -> tuple[int, float, int]:
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
                return exact, math.inf, 2**64 - 1
            exact += 1
            continue
        if a == b:
            exact += 1
            continue
        if not (math.isfinite(a) and math.isfinite(b)):
            return exact, math.inf, 2**64 - 1
        scale = max(abs(a), abs(b), sys.float_info.min)
        max_relative = max(max_relative, abs(a - b) / scale)
        max_ulp = max(max_ulp, abs(ordered_double(a) - ordered_double(b)))
    return exact, max_relative, max_ulp


def scaled_data(full: dict[str, object], size: int) -> dict[str, object]:
    total = full.get("ndatapoints")
    if not isinstance(total, int) or total < size:
        raise RuntimeError(f"ndatapoints={total!r} cannot supply N={size}")
    missing = [name for name in ROW_FIELDS if name not in full]
    if missing:
        raise RuntimeError(f"ctsem data are missing row fields: {missing}")
    result = dict(full)
    for name in ROW_FIELDS:
        rows = full[name]
        if not isinstance(rows, list) or len(rows) != total:
            raise RuntimeError(
                f"ctsem field {name} has length "
                f"{len(rows) if isinstance(rows, list) else 'non-array'}, "
                f"expected {total}"
            )
        result[name] = rows[:size]
    result["ndatapoints"] = size
    return result


def parse_graph(output: str) -> tuple[int, int, int]:
    lines = output.splitlines()
    match = GRAPH_HEADER.match(lines[0] if lines else "")
    if not match:
        raise RuntimeError("dump_ops did not produce its graph header")
    slots, ops = int(match.group(1)), int(match.group(2))
    scans = sum(
        len(parts) > 1 and parts[1] == "SCAN"
        for line in lines[1:]
        if (parts := line.split())
    )
    return slots, ops, scans


def parse_benchmark(output: str) -> tuple[float, float, int]:
    fields = output.strip().splitlines()[-1].split()
    if len(fields) != 4:
        raise RuntimeError("bench_grad did not produce its four-field result")
    return float(fields[0]), float(fields[2]), int(fields[3])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mir", required=True, type=pathlib.Path)
    parser.add_argument("--data", required=True, type=pathlib.Path,
                        help="complete issue-248 ctsem.data.json")
    parser.add_argument("--model", type=pathlib.Path, default=pathlib.Path("ctsm.stan"),
                        help="report identity for stanli_check; --mir avoids recompiling it")
    parser.add_argument("--build", type=pathlib.Path, default=pathlib.Path("build-rel"))
    parser.add_argument("--out", type=pathlib.Path,
                        help="artifact directory (default: a retained temporary directory)")
    parser.add_argument("--sizes", type=int, nargs="+", default=list(SIZES))
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--max-ulp", type=int, default=0,
                        help="explicit semantic ceiling; default requires bitwise parity")
    parser.add_argument(
        "--require-scan-at",
        type=int,
        default=33,
        help="if this N is selected, require OP_SCAN and fewer graph ops (default: 33)",
    )
    args = parser.parse_args()

    if args.repetitions < 1:
        parser.error("--repetitions must be positive")
    if args.max_ulp < 0:
        parser.error("--max-ulp must be nonnegative")
    required = {
        "MIR": args.mir,
        "data": args.data,
        "stanli_check": args.build / "stanli_check",
        "dump_ops": args.build / "dump_ops",
        "bench_grad": args.build / "bench_grad",
    }
    for label, path in required.items():
        if not path.is_file():
            parser.error(f"{label} not found: {path}")

    out = args.out or pathlib.Path(tempfile.mkdtemp(prefix="stanli-ctsem-scan."))
    out.mkdir(parents=True, exist_ok=True)
    full = json.loads(args.data.read_text())
    data_paths: dict[int, pathlib.Path] = {}
    for size in args.sizes:
        path = out / f"ctsem.data.{size}.json"
        path.write_text(json.dumps(scaled_data(full, size), indent=2) + "\n")
        data_paths[size] = path

    print(f"artifacts: {out}")
    print(f"MIR SHA-256: {sha256(args.mir)}")
    print(f"data SHA-256: {sha256(args.data)}")
    print(
        "N\tmode\tscans\tops\tslots\tslot_elems\tprep_s\t"
        "gradient_ns\tforward_ns\tn_params\tmax_rel\tmax_ulp"
    )
    results: dict[tuple[int, str], dict[str, float | int]] = {}

    for index, size in enumerate(args.sizes):
        data = data_paths[size]
        check: dict[str, str] = {}
        # Alternate the first arm so warm caches do not always favor one mode.
        modes = ("scan", "unrolled") if index % 2 == 0 else ("unrolled", "scan")
        for mode in modes:
            enabled = mode == "scan"
            env = environment(enabled)
            check[mode], _, _ = run(
                [str(required["stanli_check"]), str(args.model), str(data),
                 "--mir", str(args.mir), "--point", "0"],
                env=env,
                stdout_path=out / f"n{size}.{mode}.check.out",
                stderr_path=out / f"n{size}.{mode}.check.err",
            )
            dumped, _, _ = run(
                [str(required["dump_ops"]), str(args.mir), str(data),
                 str(2**31 - 1)],
                env=env,
                stdout_path=out / f"n{size}.{mode}.ops.out",
                stderr_path=out / f"n{size}.{mode}.ops.err",
            )
            slots, ops, scans = parse_graph(dumped)
            prep_env = dict(env)
            prep_env["STANLI_PROFILE_PREP"] = "1"
            prep_out, prep_err, _ = run(
                [str(required["bench_grad"]), str(args.mir), str(data), "--prep"],
                env=prep_env,
                stdout_path=out / f"n{size}.{mode}.prep.out",
                stderr_path=out / f"n{size}.{mode}.prep.err",
            )
            slot_match = SLOT_ELEMS.search(prep_err)
            if not slot_match:
                raise RuntimeError("preparation profile did not report slot_elems")
            prep_seconds = float(prep_out.strip().splitlines()[-1].split()[0])
            timings = []
            for repetition in range(args.repetitions):
                timed_out, _, _ = run(
                    [str(required["bench_grad"]), str(args.mir), str(data), "1"],
                    env=env,
                    stdout_path=out / f"n{size}.{mode}.bench{repetition}.out",
                    stderr_path=out / f"n{size}.{mode}.bench{repetition}.err",
                )
                timings.append(parse_benchmark(timed_out))
            gradient_ns = statistics.median(t[0] for t in timings)
            forward_ns = statistics.median(t[1] for t in timings)
            n_params = timings[0][2]
            results[size, mode] = {
                "scans": scans,
                "ops": ops,
                "slots": slots,
                "slot_elems": int(slot_match.group(1)),
                "prep_s": prep_seconds,
                "gradient_ns": gradient_ns,
                "forward_ns": forward_ns,
                "n_params": n_params,
            }

        exact, max_relative, max_ulp = compare_ok_lines(
            check["scan"], check["unrolled"]
        )
        n_values = len(check["scan"].strip().split()) - 1
        if max_ulp > args.max_ulp:
            raise RuntimeError(
                f"N={size}: scan/unrolled differ by {max_ulp} ULP "
                f"(allowed {args.max_ulp}); {exact}/{n_values} values exact"
            )
        if int(results[size, "unrolled"]["scans"]) != 0:
            raise RuntimeError(f"N={size}: STANLI_NO_SCAN=1 still emitted OP_SCAN")
        if int(results[size, "scan"]["n_params"]) != 580:
            raise RuntimeError(
                f"N={size}: expected 580 unconstrained parameters, got "
                f"{results[size, 'scan']['n_params']}"
            )
        for mode in ("scan", "unrolled"):
            row = results[size, mode]
            print(
                f"{size}\t{mode}\t{row['scans']}\t{row['ops']}\t{row['slots']}\t"
                f"{row['slot_elems']}\t{row['prep_s']:.6f}\t"
                f"{row['gradient_ns']:.1f}\t{row['forward_ns']:.1f}\t"
                f"{row['n_params']}\t{max_relative:.3e}\t{max_ulp}"
            )

    required_size = args.require_scan_at
    if required_size in args.sizes:
        if int(results[required_size, "scan"]["scans"]) == 0:
            raise RuntimeError(
                f"N={required_size}: the scan-enabled path did not emit OP_SCAN"
            )
        if int(results[required_size, "scan"]["ops"]) >= int(
            results[required_size, "unrolled"]["ops"]
        ):
            raise RuntimeError(
                f"N={required_size}: OP_SCAN did not reduce main-graph ops"
            )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"bench_ctsem_scan: {error}", file=sys.stderr)
        raise SystemExit(1)
