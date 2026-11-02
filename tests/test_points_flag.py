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


def splitmix_u(i):
    mask = (1 << 64) - 1
    z = (i + 0x9E3779B97F4A7C15) & mask
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & mask
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & mask
    z ^= z >> 31
    return (z >> 11) * 2.0 ** -53


# gq_scalar_rng_complete draws RNG columns, alg_noparam has no parameters at
# all, ar1 has 24 of them.
for model in ("gq_scalar_rng_complete", "alg_noparam", "ar1"):
    together = split_points(run(model, "--points", "0,3,1"))
    assert list(together) == [0, 3, 1], (model, list(together))
    for k, text in together.items():
        alone = run(model, "--point", str(k))
        assert text == alone, (model, k, text, alone)

for variant, low, width in ((3, -1.5, 3.0), (4, 0.1, 0.8)):
    ar1 = run("ar1", "--point", str(variant))
    wavals = [l for l in ar1.splitlines()
              if l.startswith("WAVALS")][0].split()[1:]
    got = [float(v) for v in wavals]
    assert got == [low + width * splitmix_u(i) for i in range(len(got))], \
        (variant, got)

print("points flag: framing, per-point equality, RNG reseed, variants 3, 4")
