"""The data-loop memo must not change a model's log density or gradient.

lag.stan carries a lag through an inner loop: a conditional target reads it
before the loop rewrites it on every row, the shape of an autoregressive
lag. alias_then_write.stan copies a vector inside a memoized loop and then
writes the original in place on the next row. Frames are forced because the
memo only runs when frames are recorded. Each model is swept over several
points and back to the first, and every log density and gradient on the way
must match memo off, since a stale memo shows only on a later evaluation.

inplace_logged_read.stan and inplace_logged_matrix.stan write a data-valued
local in place after a parameter-dependent call has read it. They run with
the shipped selector and with the retained path forced, and every swept
point must match STANLI_STRUCTURED_LOOPS=0.
"""
import os
from pathlib import Path
import re
import subprocess
import sys

DIRECTORY = Path(__file__).resolve().parent / "data_loop_memo"


def gradient(check, stan, data, memo_off):
    env = dict(os.environ, STANLI_STRUCTURED_LOOPS="1", STANLI_STRUCTURED_FRAMES="1",
               STANLI_STRUCTURED_LOOP_DIAGNOSTICS="1")
    if memo_off:
        env["STANLI_NO_DATA_LOOP_MEMO"] = "1"
    run = subprocess.run([check, str(stan), str(data), "--point", "0", "--sweep", "4"],
                         check=True, text=True, capture_output=True, env=env)
    hits = [int(m) for m in re.findall(r"memo_hits=(\d+)", run.stderr)]
    return run.stdout.splitlines(), hits


def sweep(check, stan, data, **overrides):
    env = {k: v for k, v in os.environ.items() if not k.startswith("STANLI_STRUCTURED")}
    env["STANLI_STRUCTURED_LOOP_DIAGNOSTICS"] = "1"
    env.update(overrides)
    run = subprocess.run([check, str(stan), str(data), "--point", "0", "--sweep", "4"],
                         check=True, text=True, capture_output=True, env=env)
    lines = [line for line in run.stdout.splitlines() if line.startswith(("SWEEP_POINT ", "OK "))]
    return lines, "stanli_structured selected" in run.stderr


def check_retained_matches_off(check, name):
    stan = DIRECTORY / f"{name}.stan"
    data = stan.with_suffix(".json")
    off, engaged = sweep(check, stan, data, STANLI_STRUCTURED_LOOPS="0")
    assert not engaged, f"{name}: loops were retained with STANLI_STRUCTURED_LOOPS=0"
    assert len(off) == 6, f"{name}: expected four swept points, the return and the result: {len(off)} lines"
    for label, overrides in (("auto", {}), ("forced", {"STANLI_STRUCTURED_LOOPS": "1"})):
        on, engaged = sweep(check, stan, data, **overrides)
        assert engaged, f"{name}: the retained path did not engage under {label}"
        for at, (a, b) in enumerate(zip(on, off)):
            assert a == b, f"{name} ({label}): line {at}: {a[:70]} vs loops off {b[:70]}"
        print(f"{name}: {label} matches loops off at every swept point")


def main(check):
    for name in ("inplace_logged_read", "inplace_logged_matrix"):
        check_retained_matches_off(check, name)
    for name in ("lag", "alias_then_write"):
        stan = DIRECTORY / f"{name}.stan"
        data = stan.with_suffix(".json")
        on, hits = gradient(check, stan, data, memo_off=False)
        off, off_hits = gradient(check, stan, data, memo_off=True)
        assert hits and min(hits) > 0, f"{name}: the memo never hit ({hits})"
        assert off_hits == [0] * len(off_hits), f"{name}: memo ran while disabled"
        assert on and on[-1].startswith("OK "), f"{name}: {on[-1:]}"
        assert len(on) == 6, f"{name}: expected four swept points, the return and the result: {len(on)} lines"
        for at, (a, b) in enumerate(zip(on, off)):
            assert a == b, f"{name}: memo changes line {at}: {a[:60]} vs {b[:60]}"
        print(f"{name}: memo hits {hits}, bitwise equal to memo off")


if __name__ == "__main__":
    main(*sys.argv[1:])
