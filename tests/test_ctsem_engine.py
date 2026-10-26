"""ctsem's Kalman filter must lower to one retained loop at every data size.

The numerical oracle cannot tell a retained loop from the same computation
unrolled over every row: the answers agree and only preparation time and
memory grow with the data. The final log_prob graph is checked instead.

The post #4 entry records frames on its first gradient, which is where the
data-loop memo runs. Its diagnostics must show recorded frames and memo hits,
and its log density and gradient must equal the memo-off run bit for bit.
"""
import os
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


def first_gradient(check, stan, data, memo_off=False):
    env = dict(os.environ, STANLI_STRUCTURED_LOOP_DIAGNOSTICS="1")
    if memo_off:
        env["STANLI_NO_DATA_LOOP_MEMO"] = "1"
    run = subprocess.run([check, str(stan), str(data), "--point", "0"],
                         check=True, text=True, capture_output=True, env=env)
    return run.stdout.split(), run.stderr


def counter(stderr, line, name):
    return [int(m) for m in re.findall(rf"stanli_structured {line}:.*?\b{name}=(\d+)", stderr)]


def check_data_loop_memo(check):
    directory = Path(__file__).resolve().parent / "ctsem"
    stan = directory / "ctsem_ctsm_post4_100.stan"
    data = stan.with_suffix(".json")
    on, on_log = first_gradient(check, stan, data)
    off, off_log = first_gradient(check, stan, data, memo_off=True)
    frames = counter(on_log, "frames", "frames")
    assert frames and min(frames) > 0, f"{stan.stem}: no frames recorded"
    hits = counter(on_log, "data_loop_memo", "memo_hits")
    misses = counter(on_log, "data_loop_memo", "memo_misses")
    assert hits and min(hits) > 0, f"{stan.stem}: the data-loop memo never hit"
    assert all(h > m for h, m in zip(hits, misses)), f"{stan.stem}: hits {hits}, misses {misses}"
    assert on[0] == "OK" and len(on) == 582, f"{stan.stem}: {on[:2]}"
    assert on == off, f"{stan.stem}: memo changes the log density or gradient"
    assert counter(off_log, "data_loop_memo", "memo_hits") == [0] * len(hits)
    print(f"{stan.stem}: frames {frames}, memo hits {hits}, misses {misses}, bitwise equal to memo off")


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
    check_data_loop_memo(check)


if __name__ == "__main__":
    main(*sys.argv[1:])
