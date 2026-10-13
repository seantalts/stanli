"""ctsem's Kalman filter must lower to one retained loop at every data size.

The numerical oracle cannot tell a retained loop from the same computation
unrolled over every row: the answers agree and only preparation time and
memory grow with the data. The final log_prob graph is checked instead.
"""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
STAGE = "log_prob:reduce"
MAX_OPS = 2000


def final_graph(check, stan, data):
    out = subprocess.run([check, str(stan), str(data), f"--dump-passes={STAGE}"],
                         check=True, text=True, capture_output=True).stdout
    lines = out.splitlines()
    first = lines.index(f";; {STAGE}")
    last = lines.index(f";; end {STAGE}", first)
    return lines[first + 1:last]


def main(check):
    directory = ROOT / "tests" / "ctsem"
    small = directory / "ctsem_ctsm.stan"
    large = directory / "ctsem_ctsm_400.stan"
    assert small.read_bytes() == large.read_bytes(), "the two entries share one model"
    for stan in (small, large):
        graph = final_graph(check, stan, stan.with_suffix(".json"))
        ops = int(re.match(r"ops=(\d+) ", graph[0]).group(1))
        loops = sum(" = LOOP " in line for line in graph)
        assert loops == 1, f"{stan.stem}: {loops} retained loops, expected 1 ({ops} ops)"
        assert ops <= MAX_OPS, f"{stan.stem}: {ops} lowered ops, expected at most {MAX_OPS}"
        print(f"{stan.stem}: {loops} loop, {ops} ops")


if __name__ == "__main__":
    main(*sys.argv[1:])
