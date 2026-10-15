"""Peak memory and lane-tile size of compiling and evaluating a region map with a very large body."""
import os
import re
import resource
import subprocess
import sys

LIMIT_MB = 120
FWD_REGS = 6577
ADJ_CELLS = 267


def main(stanli_check):
    run = subprocess.run(
        [stanli_check, "tests/cogmod/cm_invgaussian.stan",
         "tests/cogmod/cm_invgaussian.json"],
        check=True, capture_output=True, text=True,
        env={**os.environ, "STANLI_REGION_MAP_DIAGNOSTICS": "1"})
    peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    peak_mb = peak / 1e6 if sys.platform == "darwin" else peak / 1e3
    assert peak_mb < LIMIT_MB, f"peak {peak_mb:.0f} MB exceeds {LIMIT_MB} MB"
    line = next((l for l in (run.stdout + run.stderr).splitlines()
                 if "stanli_region_map selected" in l), "")
    sizes = re.search(r"lanes=save .*adj_cells=(\d+) fwd_regs=(\d+)", line)
    assert sizes, f"no lane-save region map in: {line!r}"
    assert (int(sizes[2]), int(sizes[1])) == (FWD_REGS, ADJ_CELLS), line


if __name__ == "__main__":
    main(sys.argv[1])
