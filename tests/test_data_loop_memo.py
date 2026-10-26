"""The data-loop memo must not change a model's log density or gradient.

lag.stan carries a lag through an inner loop: a conditional target reads it
before the loop rewrites it on every row, the shape of an autoregressive
lag. Frames are forced because the memo only runs when frames are recorded.
"""
import os
from pathlib import Path
import re
import subprocess
import sys

DIRECTORY = Path(__file__).resolve().parent / "data_loop_memo"


def first_gradient(check, stan, data, memo_off):
    env = dict(os.environ, STANLI_STRUCTURED_LOOPS="1", STANLI_STRUCTURED_FRAMES="1",
               STANLI_STRUCTURED_LOOP_DIAGNOSTICS="1")
    if memo_off:
        env["STANLI_NO_DATA_LOOP_MEMO"] = "1"
    run = subprocess.run([check, str(stan), str(data), "--point", "0"],
                         check=True, text=True, capture_output=True, env=env)
    hits = [int(m) for m in re.findall(r"memo_hits=(\d+)", run.stderr)]
    return run.stdout.split(), hits


def main(check):
    stan = DIRECTORY / "lag.stan"
    data = stan.with_suffix(".json")
    on, hits = first_gradient(check, stan, data, memo_off=False)
    off, off_hits = first_gradient(check, stan, data, memo_off=True)
    assert hits and min(hits) > 0, f"{stan.stem}: the memo never hit ({hits})"
    assert off_hits == [0] * len(off_hits), f"{stan.stem}: memo ran while disabled"
    assert on[0] == "OK", f"{stan.stem}: {on[:2]}"
    assert on == off, f"{stan.stem}: memo changes the log density or gradient: {on[:3]} vs {off[:3]}"
    print(f"{stan.stem}: memo hits {hits}, bitwise equal to memo off")


if __name__ == "__main__":
    main(*sys.argv[1:])
