#!/usr/bin/env python3
"""`~` drops the terms CmdStan drops.

Under `~`, Stan Math keeps a density term only when one of the arguments it
depends on is a parameter. Each case pairs a `~` statement with the same
log density written out by hand with only the terms CmdStan keeps, and the
two must agree in lp and gradient.
"""
import json
import pathlib
import subprocess
import sys
import tempfile

check = str(pathlib.Path(sys.argv[1]).resolve())

DATA = {"y": [0.3, -1.2], "nu": 4.5, "S": [[2.0, 0.5], [0.5, 1.0]]}

MST = """
data {
  vector[2] y;
  real nu;
  matrix[2, 2] S;
}
transformed data {
  matrix[2, 2] L = cholesky_decompose(S);
}
parameters {
  vector[2] mu;
  real<lower=0> nu_p;
}
model {
  nu_p ~ gamma(2, 0.1);
  %s
}
"""

QUAD = "log1p(quad_form(inverse(S), y - mu) / {nu})"
NU_TERMS = ("lgamma(0.5 * (nu_p + 2)) - lgamma(0.5 * nu_p) "
            "- log(nu_p)")

CASES = [
    ("multi_student_t, nu and Sigma data",
     "y ~ multi_student_t(nu, mu, S);",
     "target += -0.5 * (nu + 2) * " + QUAD.format(nu="nu") + ";"),
    ("multi_student_t_cholesky, nu and L data",
     "y ~ multi_student_t_cholesky(nu, mu, L);",
     "target += -0.5 * (nu + 2) * " + QUAD.format(nu="nu") + ";"),
    ("multi_student_t, nu a parameter",
     "y ~ multi_student_t(nu_p, mu, S);",
     "target += " + NU_TERMS + " - 0.5 * (nu_p + 2) * "
     + QUAD.format(nu="nu_p") + ";"),
    ("multi_student_t_cholesky, nu a parameter",
     "y ~ multi_student_t_cholesky(nu_p, mu, L);",
     "target += " + NU_TERMS + " - 0.5 * (nu_p + 2) * "
     + QUAD.format(nu="nu_p") + ";"),
]


def lp_grad(statement):
    with tempfile.TemporaryDirectory(prefix="lp_propto_") as tmp:
        model = pathlib.Path(tmp) / "model.stan"
        model.write_text(MST % statement)
        data = pathlib.Path(tmp) / "data.json"
        data.write_text(json.dumps(DATA))
        proc = subprocess.run([check, str(model), str(data), "--point", "1"],
                              capture_output=True, text=True, timeout=120)
    for line in proc.stdout.splitlines():
        if line.startswith("OK "):
            return [float(x) for x in line.split()[1:]]
    raise AssertionError(f"{statement}: {proc.stdout[-500:]}{proc.stderr}")


failures = 0
for name, tilde, by_hand in CASES:
    got, want = lp_grad(tilde), lp_grad(by_hand)
    ok = len(got) == len(want) and all(
        abs(a - b) <= 1e-12 * max(1.0, abs(a), abs(b))
        for a, b in zip(got, want))
    print(f"{'ok' if ok else 'FAIL'} {name}: ~ {got}, by hand {want}")
    failures += not ok
sys.exit(1 if failures else 0)
