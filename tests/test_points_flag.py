#!/usr/bin/env python3
"""stanli_check --points: one compile, same numbers as separate --point runs."""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from cmdstan_ref import split_points  # noqa: E402

check = str(pathlib.Path(sys.argv[1]).resolve())
FIX = ROOT / "tests" / "fixtures"


def run(model, *flags):
    proc = subprocess.run(
        [check, str(FIX / (model + ".stan")), str(FIX / (model + ".json")),
         "--mir", str(FIX / (model + ".tmir.sexp")), "--wa-values", *flags],
        capture_output=True, text=True, timeout=120)
    assert proc.returncode == 0, (model, proc.stdout, proc.stderr)
    return proc.stdout


def eval_point3(i):
    mask = (1 << 64) - 1
    z = (i + 0x9E3779B97F4A7C15) & mask
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & mask
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & mask
    z ^= z >> 31
    return -1.5 + 3.0 * ((z >> 11) * 2.0 ** -53)


# gq_scalar_rng_complete draws RNG columns, alg_noparam has no parameters at
# all, ar1 has 24 of them.
for model in ("gq_scalar_rng_complete", "alg_noparam", "ar1"):
    together = split_points(run(model, "--points", "0,3,1"))
    assert list(together) == [0, 3, 1], (model, list(together))
    for k, text in together.items():
        alone = run(model, "--point", str(k))
        assert text == alone, (model, k, text, alone)

ar1 = run("ar1", "--point", "3")
wavals = [l for l in ar1.splitlines() if l.startswith("WAVALS")][0].split()[1:]
got = [float(v) for v in wavals]
assert got == [eval_point3(i) for i in range(len(got))], got

print("points flag: framing, per-point equality, RNG reseed, variant 3")
