#!/usr/bin/env python3
"""`a[:] = ...` onto a declared zero-length local resizes it the same way
CmdStan resizes a bare `a = ...`: replays the fixture against a CmdStan
reference recorded once with tools/cmdstan_ref.py (see
viewc_zero_length_whole_assignment_resize.ref.json), rather than requiring a
CmdStan checkout on every run.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
FIXTURE = REPO / "tests/fixtures/viewc_zero_length_whole_assignment_resize.stan"
REFERENCE = REPO / "tests/fixtures/viewc_zero_length_whole_assignment_resize.ref.json"


def source_digest(path):
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def parse_ok(output):
    for line in output.splitlines():
        fields = line.split()
        if fields and fields[0] == "OK":
            return float(fields[1]), [float(x) for x in fields[2:]]
    raise ValueError("no OK line in stanli_check output:\n" + output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build-rel")
    args = parser.parse_args()

    reference = json.loads(REFERENCE.read_text())
    if reference["source_sha256"] != source_digest(FIXTURE):
        raise ValueError(FIXTURE.name + ": source changed; re-record the "
                         "CmdStan reference with tools/cmdstan_ref.py")

    check = (args.build / "stanli_check").resolve()
    with tempfile.TemporaryDirectory() as tmp:
        data_path = pathlib.Path(tmp) / "data.json"
        data_path.write_text(json.dumps(reference["data"]), encoding="utf-8")
        result = subprocess.run(
            [str(check), str(FIXTURE), str(data_path),
             "--point", str(reference["point"])],
            cwd=REPO, text=True, capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError("stanli_check failed:\n" + result.stdout + result.stderr)
    lp, grad = parse_ok(result.stdout)

    if not math.isclose(lp, reference["lp"], rel_tol=1e-9, abs_tol=1e-12):
        raise ValueError(f"lp mismatch: stanli={lp!r} CmdStan={reference['lp']!r}")
    if len(grad) != len(reference["grad"]):
        raise ValueError("gradient width differs from the CmdStan reference")
    for got, want in zip(grad, reference["grad"]):
        if not math.isclose(got, want, rel_tol=1e-9, abs_tol=1e-12):
            raise ValueError(f"gradient mismatch: stanli={got!r} CmdStan={want!r}")

    print(f"PASS {FIXTURE.name}: lp={lp!r} matches CmdStan's recorded {reference['lp']!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
