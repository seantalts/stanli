#!/usr/bin/env python3
"""write_array cases checked against what CmdStan's write_array does.

Each case runs through the compiled write_array graph, with the interpreter
fallback refused, and again through the MIR interpreter, and both must give
the expected row or the expected error.
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

check = str(pathlib.Path(sys.argv[1]).resolve())

CASES = [
    # CmdStan's write_array starts from lp__ = 0 and never adds the
    # Jacobian, so target() in transformed parameters reads 0.
    ("target_in_transformed_parameters", """
parameters {
  real<lower=0> y;
}
transformed parameters {
  real z = target();
}
model {
  y ~ normal(0, 1);
}
""", {}, {"names": ["y", "z"], "z": 0.0}),
    ("generated_quantity_keeps_declared_size", """
parameters {
  real y;
}
model {
  y ~ normal(0, 1);
}
generated quantities {
  matrix[2, 3] z = [[1, 2, 3, 4], [4, 5, 6, 8]];
}
""", {}, {"error": "matrix assign columns: assigning variable z (3) and "
                   "right hand side columns (4) must match in size"}),
    ("unit_vector_zero_norm", """
parameters {
  vector[2] p;
}
transformed parameters {
  vector[2] u = unit_vector_constrain(p - p);
}
model {
  p ~ normal(0, 1);
}
""", {}, {"error": "unit_vector_constrain: norm is 0, but must be positive "
                   "finite"}),
    # Not CmdStan's behavior: stanli cannot evaluate laplace_latent_rng, and
    # says so rather than naming the function argument as unknown.
    ("unsupported_higher_order_call", """
functions {
  real ll(vector theta, real eta) {
    return normal_lpdf(theta | 0, eta);
  }
  matrix K(int n) {
    return identity_matrix(n);
  }
}
parameters {
  real<lower=0> eta;
}
model {
  eta ~ lognormal(0, 1);
}
generated quantities {
  vector[2] theta = laplace_latent_rng(ll, (eta,), 1, K, (2,));
}
""", {}, {"error": "unsupported", "interp_only": True}),
]


def run(source, data, interp):
    with tempfile.TemporaryDirectory(prefix="wa_semantics_") as tmp:
        model = pathlib.Path(tmp) / "model.stan"
        model.write_text(source)
        data_file = pathlib.Path(tmp) / "data.json"
        data_file.write_text(json.dumps(data))
        env = dict(os.environ)
        env.pop("STANLI_WA_FORCE_INTERP", None)
        env.pop("STANLI_NO_INTERPRETER", None)
        env["STANLI_WA_FORCE_INTERP" if interp else "STANLI_NO_INTERPRETER"] = "1"
        proc = subprocess.run(
            [check, str(model), str(data_file), "--point", "1",
             "--wa-values"], capture_output=True, text=True, timeout=120,
            env=env)
    lines = {}
    for line in proc.stdout.splitlines():
        head, _, tail = line.partition(" ")
        lines[head] = tail
    return lines


failures = 0
for name, source, data, want in CASES:
    for interp in (True,) if want.get("interp_only") else (False, True):
        label = f"{name} ({'interp' if interp else 'graph'})"
        got = run(source, data, interp)
        names = got.get("WANAMES", "")
        if "error" in want:
            ok = names.startswith("FAIL") and want["error"] in names
        elif names.startswith("FAIL") or "WAVALS" not in got:
            ok = False
        else:
            columns = names.split(",")
            values = dict(zip(columns, map(float, got["WAVALS"].split())))
            ok = columns == want["names"] and all(
                values[k] == v for k, v in want.items() if k != "names")
        if not ok:
            failures += 1
            print(f"FAIL {label}: want {want}, got WANAMES {names!r} "
                  f"WAVALS {got.get('WAVALS')!r}")
        else:
            print(f"ok {label}")
sys.exit(1 if failures else 0)
