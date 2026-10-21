#!/usr/bin/env python3
"""Record CmdStan references for tests/fixtures/cmdstan_replay/*.stan.

Each fixture NAME.stan is built against the pinned CmdStan with the stock
stanc, evaluated at stanli_check's deterministic points, and written to
NAME.ref.json next to it for tests/test_cmdstan_replay.py to replay. The
data comes from NAME.data.json when present.

  python3 tools/record_replay_refs.py [--cmdstan DIR] [NAME ...]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import cmdstan_ref  # noqa: E402

FIXTURES = REPO / "tests/fixtures/cmdstan_replay"
POINTS = (0, 1, 2)


def source_digest(path):
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def record(stan, cmdstan):
    data_path = stan.with_suffix(".data.json")
    data = json.loads(data_path.read_text()) if data_path.exists() else {}
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        (work / "data.json").write_text(json.dumps(data), encoding="utf-8")
        exe, err = cmdstan_ref.build_reference(
            cmdstan, work, stan, REPO / "tools/ref_driver.cpp",
            REPO / "deps/stanc3/stanc", sundials=False)
        if exe is None:
            raise RuntimeError(f"{stan.name}: {err}")
        points = []
        for p in POINTS:
            out = subprocess.run([str(exe), str(work / "data.json"), str(p)],
                                 capture_output=True, text=True).stdout
            fields = cmdstan_ref._result_line(out)
            if fields[:1] != ["OK"]:
                raise RuntimeError(f"{stan.name}: CmdStan failed at point {p}")
            vals = [float(x) for x in fields[1:]]
            points.append({"point": p, "lp": vals[0], "grad": vals[1:]})
    return {"source_sha256": source_digest(stan), "data": data,
            "points": points}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmdstan", type=pathlib.Path,
                        default=REPO / "deps/cmdstan")
    parser.add_argument("names", nargs="*")
    args = parser.parse_args()
    cmdstan = args.cmdstan.resolve()
    stans = ([FIXTURES / f"{n}.stan" for n in args.names] if args.names
             else sorted(p for p in FIXTURES.glob("*.stan")))
    for stan in stans:
        ref = record(stan, cmdstan)
        out = stan.with_suffix(".ref.json")
        out.write_text(json.dumps(ref, indent=1) + "\n")
        print(f"recorded {out.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
