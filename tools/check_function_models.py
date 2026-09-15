#!/usr/bin/env python3
"""Compare the ten function models with recorded CmdStan answers.

--record compiles every reference from the pinned CmdStan checkout, records
all three points independently of stanli's answers, then runs the same gate
as ordinary replay. No compile failure, missing output or mismatched shape is
skipped. Source hashes prevent stale references from passing after an edit.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import gzip
import hashlib
import json
import math
import pathlib
import platform
import subprocess
import sys

from cmdstan_ref import compile_cmd

REPO = pathlib.Path(__file__).resolve().parents[1]
FIXTURES = REPO / "tests/function_coverage"
REFERENCE = FIXTURES / "references.json.gz"


def run(argv):
    result = subprocess.run([str(x) for x in argv], cwd=REPO,
                            text=True, capture_output=True, timeout=600)
    if result.returncode:
        raise RuntimeError(f"{argv[0]} failed ({result.returncode}):\n"
                           + result.stdout[-3000:] + result.stderr[-3000:])
    return result.stdout


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_digest(path):
    # Git may check text files out as CRLF on Windows. Only normalize that
    # checkout transformation; compiler binaries still use the raw digest.
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def parse(text):
    fields = {}
    for line in text.splitlines():
        head, _, tail = line.partition(" ")
        if head == "OK":
            fields["lp_grad"] = [float(x) for x in tail.split()]
        elif head == "WANAMES":
            if tail.startswith("FAIL"):
                raise ValueError(line)
            fields["names"] = tail.split(",")
        elif head == "WAVALS":
            fields["values"] = [float(x) for x in tail.split()]
    if set(fields) != {"lp_grad", "names", "values"}:
        raise ValueError("Missing numeric or write-array output: " + text[-500:])
    if not fields["lp_grad"] or len(fields["names"]) != len(fields["values"]):
        raise ValueError("Malformed reference output")
    if not all(math.isfinite(x) for k in ("lp_grad", "values") for x in fields[k]):
        raise ValueError("Function probes must produce finite outputs")
    return fields


def record_one(name, args):
    source = FIXTURES / (name + ".stan")
    identity = hashlib.sha256((source_digest(source) + source_digest(REPO / "tools/ref_driver.cpp")
                              + json.dumps(args.toolchain, sort_keys=True)).encode()).hexdigest()[:16]
    cache = args.build / "function-refs" / (name + "-" + identity)
    cache.mkdir(parents=True, exist_ok=True)
    hpp, exe = cache / "model.hpp", cache / "reference"
    if not exe.exists():
        run([args.stanc, source, f"--o={hpp}"])
        run(compile_cmd(args.cmdstan, hpp, REPO / "tools/ref_driver.cpp", exe))
    points = [parse(run([exe, args.data, p])) for p in range(3)]
    print(f"recorded {name}", flush=True)
    return name, {"source_sha256": source_digest(source), "points": points}


def compare(name, reference, args):
    source = FIXTURES / (name + ".stan")
    if reference["source_sha256"] != source_digest(source):
        raise ValueError(name + ": source changed; re-record the CmdStan reference")
    if len(reference["points"]) != 3:
        raise ValueError(name + ": expected exactly three reference points")
    worst = 0.0
    for point, want in enumerate(reference["points"]):
        command = [args.build / "stanli_check", source, args.data,
                   "--point", point]
        got = parse(run(command + ["--wa-values"]))
        if got["names"] != want["names"]:
            raise ValueError(name + ": output column names/order differ")
        for field in ("lp_grad", "values"):
            if len(got[field]) != len(want[field]):
                raise ValueError(name + ": " + field + " width differs")
            for i, (a, b) in enumerate(zip(got[field], want[field])):
                error = abs(a-b) / max(1.0, abs(a), abs(b))
                worst = max(worst, error)
                # Same scaled gate as the ordinary CmdStan corpus replay.
                if error > 1e-9:
                    label = want["names"][i] if field == "values" else str(i)
                    raise ValueError(f"{name} point {point} {field}[{label}]: "
                                     f"stanli={a:.17g}, CmdStan={b:.17g}, "
                                     f"scaled error={error:.3g}")
    print(f"PASS {name}: 3 points, lp/gradient/all outputs; worst={worst:.3g}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build-rel")
    parser.add_argument("--stanc", type=pathlib.Path, default=REPO / "deps/stanc3/stanc")
    parser.add_argument("--cmdstan", type=pathlib.Path, default=REPO / "deps/cmdstan")
    parser.add_argument("--record", action="store_true")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    args.build, args.stanc, args.cmdstan = (p.resolve() for p in
                                          (args.build, args.stanc, args.cmdstan))
    args.data = FIXTURES / "empty.json"
    manifest = json.loads((FIXTURES / "manifest.json").read_text())
    names = sorted(manifest["models"])
    if len(names) != 10:
        raise ValueError("Expected exactly ten function-coverage models")
    if args.record:
        args.toolchain = {
            "stanc_sha256": digest(args.stanc),
            "stanc_version": run([args.stanc, "--version"]).strip(),
            "compiler": run(["clang++", "--version"]).splitlines()[0],
            "platform": platform.system() + " " + platform.machine(),
            "flags": "-std=c++17 -O1 -ffp-contract=off -D_REENTRANT -DBOOST_DISABLE_ASSERTS",
        }
        for key, path in {"cmdstan": args.cmdstan,
                          "stan": args.cmdstan / "stan",
                          "math": args.cmdstan / "stan/lib/stan_math"}.items():
            args.toolchain[key + "_commit"] = run(["git", "-C", path, "rev-parse", "HEAD"]).strip()
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            models = dict(pool.map(lambda n: record_one(n, args), names))
        recorded = {"schema": 1, "toolchain": args.toolchain,
                    "models": models}
        payload = json.dumps(recorded, indent=2, sort_keys=True, allow_nan=False) + "\n"
        REFERENCE.write_bytes(gzip.compress(payload.encode(), mtime=0))
    references = json.loads(gzip.decompress(REFERENCE.read_bytes()))
    if references.get("schema") != 1:
        raise ValueError("Unknown function reference schema")
    if set(references["models"]) != set(names):
        raise ValueError("Reference model set differs from the manifest")
    failures = []
    for name in names:
        try:
            compare(name, references["models"][name], args)
        except (RuntimeError, ValueError) as error:
            failures.append(str(error))
            print("FAIL", error, flush=True)
    if failures:
        return 1
    print(f"All {len(names)} models passed; {manifest['name_count']} named functions/operators covered.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
