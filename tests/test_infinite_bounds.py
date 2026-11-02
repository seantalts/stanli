#!/usr/bin/env python3
"""Unconstraining against an infinite bound is the identity, as in Stan Math.

lb_free with lb = -inf and ub_free with ub = +inf return y; lub_free with
one infinite bound is the other one-sided transform. Each case pairs a call
with the expression Stan Math reduces it to, and the two must agree in lp
and gradient.
"""
import json
import pathlib
import subprocess
import sys
import tempfile

check = str(pathlib.Path(sys.argv[1]).resolve())

MODEL = """
transformed data {
  real inf = positive_infinity();
}
parameters {
  real p;
  vector[2] v;
}
model {
  p ~ normal(0, 1);
  v ~ normal(0, 1);
  %s
}
"""

CASES = [
    ("upper bound +inf", "target += upper_bound_unconstrain(p, inf);",
     "target += p;"),
    ("lower bound -inf", "target += lower_bound_unconstrain(p, -inf);",
     "target += p;"),
    ("vector upper bound +inf",
     "target += sum(upper_bound_unconstrain(v, rep_vector(inf, 2)));",
     "target += sum(v);"),
    ("both bounds infinite",
     "target += lower_upper_bound_unconstrain(p, -inf, inf);",
     "target += p;"),
    ("lower bound -inf, upper finite",
     "target += lower_upper_bound_unconstrain(5 - exp(p), -inf, 5);",
     "target += log(5 - (5 - exp(p)));"),
    ("upper bound +inf, lower finite",
     "target += lower_upper_bound_unconstrain(5 + exp(p), 5, inf);",
     "target += log((5 + exp(p)) - 5);"),
]


def lp_grad(statement):
    with tempfile.TemporaryDirectory(prefix="infinite_bounds_") as tmp:
        model = pathlib.Path(tmp) / "model.stan"
        model.write_text(MODEL % statement)
        data = pathlib.Path(tmp) / "data.json"
        data.write_text(json.dumps({}))
        proc = subprocess.run([check, str(model), str(data), "--point", "1"],
                              capture_output=True, text=True, timeout=120)
    for line in proc.stdout.splitlines():
        if line.startswith("OK "):
            return [float(x) for x in line.split()[1:]]
    return proc.stdout.strip().splitlines()[-1:]


failures = 0
for name, call, reduced in CASES:
    got, want = lp_grad(call), lp_grad(reduced)
    ok = (len(got) == len(want) and all(isinstance(a, float) for a in got)
          and all(abs(a - b) <= 1e-12 * max(1.0, abs(a), abs(b))
                  for a, b in zip(got, want)))
    print(f"{'ok' if ok else 'FAIL'} {name}: call {got}, reduced {want}")
    failures += not ok
sys.exit(1 if failures else 0)
