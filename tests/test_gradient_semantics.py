#!/usr/bin/env python3
"""Gradients where Stan Math's reverse pass has its own edge-case behavior.

mdivide_left(A, b) with a singular A and a vector b: Stan Math's pullback
solves for b's adjoint with Eigen's vector triangular solve, which leaves a
zero right-hand side zero, so b's gradient stays finite while A's is NaN.
"""
import json
import math
import pathlib
import subprocess
import sys
import tempfile

check = str(pathlib.Path(sys.argv[1]).resolve())

MODEL = """
parameters {
  matrix[2, 2] A;
  vector[2] b;
}
transformed parameters {
  vector[2] x = mdivide_left(A - A, b);
}
model {
  b ~ normal(0, 1);
}
"""

with tempfile.TemporaryDirectory(prefix="gradient_semantics_") as tmp:
    model = pathlib.Path(tmp) / "model.stan"
    model.write_text(MODEL)
    data = pathlib.Path(tmp) / "data.json"
    data.write_text(json.dumps({}))
    proc = subprocess.run([check, str(model), str(data), "--point", "1"],
                          capture_output=True, text=True, timeout=120)
line = next((l for l in proc.stdout.splitlines() if l.startswith("OK ")), None)
assert line, proc.stdout[-500:] + proc.stderr
values = [float(v) for v in line.split()[1:]]
grad_a, grad_b = values[1:5], values[5:7]
ok = all(math.isnan(g) for g in grad_a) and all(math.isfinite(g)
                                                 for g in grad_b)
print(f"{'ok' if ok else 'FAIL'} singular mdivide_left: dA {grad_a}, db {grad_b}")
sys.exit(0 if ok else 1)
