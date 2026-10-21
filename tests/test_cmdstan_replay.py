#!/usr/bin/env python3
"""Replay stanli_check against recorded CmdStan references.

Every tests/fixtures/cmdstan_replay/NAME.stan has a NAME.ref.json recorded
once with tools/record_replay_refs.py, so a CmdStan checkout is not needed
on every run. Each fixture is replayed through the embedded compiler and
through the stock stanc, which optimizes at --O1 and so reaches the GLM
rewrites. Every recorded point must match to 1e-9.
"""
from __future__ import annotations

import argparse
import json
import math
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

from record_replay_refs import FIXTURES, source_digest  # noqa: E402


def parse_ok(output):
    for line in output.splitlines():
        fields = line.split()
        if fields and fields[0] == "OK":
            return float(fields[1]), [float(x) for x in fields[2:]]
    raise ValueError("no OK line in stanli_check output:\n" + output)


def replay(stan, check, stanc):
    reference = json.loads(stan.with_suffix(".ref.json").read_text())
    if reference["source_sha256"] != source_digest(stan):
        raise ValueError(stan.name + ": source changed; re-record with "
                         "tools/record_replay_refs.py")
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        data_path = pathlib.Path(tmp) / "data.json"
        data_path.write_text(json.dumps(reference["data"]), encoding="utf-8")
        model = pathlib.Path(tmp) / stan.name
        shutil.copyfile(stan, model)
        for want in reference["points"]:
            command = [str(check), str(model), str(data_path),
                       "--point", str(want["point"])]
            if stanc is not None:
                command += ["--stanc", str(stanc)]
            result = subprocess.run(command, cwd=REPO, text=True,
                                    capture_output=True, timeout=120)
            where = (f"{stan.name} point {want['point']}"
                     + (" (stock stanc)" if stanc else ""))
            try:
                lp, grad = parse_ok(result.stdout)
            except ValueError:
                problems.append(f"{where}: {result.stdout.strip()[-200:]}")
                continue
            if not math.isclose(lp, want["lp"], rel_tol=1e-9, abs_tol=1e-12):
                problems.append(f"{where}: lp stanli={lp!r} CmdStan={want['lp']!r}")
            if len(grad) != len(want["grad"]):
                problems.append(f"{where}: gradient width differs")
                continue
            for i, (got, exp) in enumerate(zip(grad, want["grad"])):
                if not math.isclose(got, exp, rel_tol=1e-9, abs_tol=1e-12):
                    problems.append(
                        f"{where}: grad[{i}] stanli={got!r} CmdStan={exp!r}")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build-rel")
    parser.add_argument("--stanc", type=pathlib.Path,
                        default=REPO / "deps/stanc3/stanc")
    args = parser.parse_args()
    check = (args.build / "stanli_check").resolve()
    failed = 0
    for stan in sorted(FIXTURES.glob("*.stan")):
        problems = replay(stan, check, None) + replay(stan, check, args.stanc)
        if problems:
            failed += 1
            print("FAIL " + "\n     ".join(problems))
        else:
            print(f"PASS {stan.name}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
