#!/usr/bin/env python3
"""One predeclared seven-round confirmation, unchanged binaries/calibration."""
import argparse
import json
import pathlib
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser(__doc__)
parser.add_argument("--screen", type=pathlib.Path, required=True)
parser.add_argument("--inputs", type=pathlib.Path, required=True)
parser.add_argument("--output", type=pathlib.Path, required=True)
args = parser.parse_args()
summary = json.loads((args.screen / "bench/summary.json").read_text())
flagged = {(c["name"], c["workers"]) for c in summary["cells"]
           if c["ratios"]["candidate"]["median"] < 0.97}
if not flagged:
    print("No screen cell met the predeclared confirmation trigger.")
    sys.exit(0)
cells = sorted(flagged | {("eight_schools_noncentered", 1), ("normal_1024", 4), ("hierarchical_gp", 4)})
out = args.output.resolve()
out.mkdir(parents=True)
(out / "cells.json").write_text(json.dumps(cells, indent=2))
shutil.copy2(args.screen / "calibration.json", out / "calibration.json")
calibration = json.loads((out / "calibration.json").read_text())
v = calibration["manifest"]["variants"]
command = [sys.executable, str(pathlib.Path(__file__).with_name("bench.py")), "bench",
           "--inputs", str(args.inputs.resolve()), "--system", v["system"]["library"],
           "--candidate", v["candidate"]["library"], "--output", str(out),
           "--cells", str(out / "cells.json"), "--rounds", "7"]
(out / "selection.json").write_text(json.dumps(dict(flagged=sorted(flagged), command=command), indent=2))
print("Confirming", cells, "in seven fixed rounds", flush=True)
subprocess.run(command, check=True)
