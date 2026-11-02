#!/usr/bin/env python3
"""Small programs from stanc3's good tests that stanli used to refuse.

Each case runs one program through stanli_check at point 0 and checks the
write_array values, or the message the evaluation or the data load throws.
"""
import json
import pathlib
import subprocess
import sys
import tempfile

check = str(pathlib.Path(sys.argv[1]).resolve())

CASES = [
    ("and/or of transformed data assigned to a real",
     """transformed data { int t = 1; int f = 0; }
        parameters { real y; }
        transformed parameters {
          real a = t && t;
          real o = f || t;
          real n = f && t;
        }
        model { y ~ normal(0, 1); }""",
     {}, {"values": [0.1, 1, 1, 0]}),
    ("min/max of ints assigned to a real",
     """data { int d; array[3] int a; }
        parameters { real y; }
        transformed parameters {
          real hi = max(d, 7);
          real lo = min(d, 7);
          real ahi = max(a);
          real alo = min(a);
        }
        model { y ~ normal(0, 1); }""",
     {"d": 3, "a": [4, -2, 9]}, {"values": [0.1, 7, 3, 9, -2]}),
    ("user function called as a statement in transformed data",
     """functions {
          void show(real x) { print("show ", x); }
          void show(int x) { print("show int ", x); }
          void stop(real x) { reject("stopped at ", x); }
        }
        transformed data { show(1.5); show(2); }
        parameters { real y; }
        model { y ~ normal(0, 1); }""",
     {}, {"values": [0.1], "stdout": "show 1.5\nshow int 2"}),
    ("rejection from a user function in transformed data",
     """functions { void stop(real x) { reject("stopped at ", x); } }
        transformed data { stop(2.5); }
        parameters { real y; }
        model { y ~ normal(0, 1); }""",
     {}, {"compile_fail": "stopped at 2.5"}),
    ("overloaded ODE right-hand side",
     """functions {
          vector rhs(real t, vector y, real k) { return -k * y; }
          vector rhs(real t, vector y, real k, int unused) { return k * y; }
        }
        parameters { real<lower=0> k; }
        transformed parameters {
          array[1] vector[1] decay = ode_rk45_tol(rhs, [1.0]', 0, {1.0},
                                                 1e-10, 1e-10, 10000, k);
          array[1] vector[1] grow = ode_rk45_tol(rhs, [1.0]', 0, {1.0},
                                                1e-10, 1e-10, 10000, k, 0);
        }
        model { k ~ normal(1, 1); }""",
     {}, {"close": [1.1051709180756477, 0.33115427715290885, 3.0197405529455232],
          "rtol": 1e-6}),
    ("GLMs with a row_vector design and an array outcome",
     """parameters { real b; }
        transformed parameters {
          row_vector[2] x = [1, 2];
          vector[2] beta = [b, -b]';
          real p = poisson_log_glm_lpmf({1, 0} | x, 0.5, beta);
          real l = bernoulli_logit_glm_lpmf({1, 0} | x, 0.5, beta);
          real n = binomial_logit_glm_lpmf({1, 2} | {3, 4}, x, 0.5, beta);
          real g = neg_binomial_2_log_glm_lpmf({1, 0} | x, 0.5, beta, 2.0);
        }
        model { b ~ normal(0, 1); }""",
     {}, {"close": [0.1, 1, 2, 0.1, -0.1, -2.583649395282541,
                    -1.4260305047999053, -2.300735008903504,
                    -2.3863862751181735], "rtol": 1e-13}),
    ("poisson_log_glm gradient with a row_vector design",
     """parameters { real b; }
        model {
          b ~ normal(0, 1);
          target += poisson_log_glm_lpmf({1, 0} | [1, 2], 0.5, [b, -b]');
        }""",
     {}, {"lp_grad": [-2.5886493952825407, 1.8836493952825406], "rtol": 1e-13}),
]


def run(source, data):
    with tempfile.TemporaryDirectory() as tmp:
        stan = pathlib.Path(tmp) / "model.stan"
        stan.write_text(source)
        js = pathlib.Path(tmp) / "data.json"
        js.write_text(json.dumps(data))
        proc = subprocess.run([check, str(stan), str(js), "--point", "0",
                               "--wa-values"],
                              capture_output=True, text=True, timeout=300)
    return proc.stdout + proc.stderr


failures = 0
for name, source, data, want in CASES:
    out = run(source, data)
    lines = {}
    for line in out.splitlines():
        key, _, rest = line.partition(" ")
        lines.setdefault(key, rest)
    ok = True
    if "values" in want:
        got = [float(v) for v in lines.get("WAVALS", "").split()]
        ok = "OK" in lines and len(got) == len(want["values"]) and all(
            abs(g - w) <= 1e-12 * max(1.0, abs(w))
            for g, w in zip(got, want["values"]))
    if "close" in want:
        got = [float(v) for v in lines.get("WAVALS", "").split()]
        ok = "OK" in lines and len(got) == len(want["close"]) and all(
            abs(g - w) <= want["rtol"] * abs(w)
            for g, w in zip(got, want["close"]))
    if "lp_grad" in want:
        got = [float(v) for v in lines.get("OK", "").split()]
        ok = len(got) == len(want["lp_grad"]) and all(
            abs(g - w) <= want["rtol"] * abs(w)
            for g, w in zip(got, want["lp_grad"]))
    if "eval_fail" in want:
        ok = want["eval_fail"] in lines.get("EVAL_FAIL", "")
    if "compile_fail" in want:
        ok = want["compile_fail"] in lines.get("COMPILE_FAIL", "")
    if "stdout" in want:
        ok = ok and want["stdout"] in out
    if not ok:
        failures += 1
        print(f"FAIL {name}\n{out}")
print(f"{len(CASES) - failures}/{len(CASES)} cases OK")
sys.exit(1 if failures else 0)
