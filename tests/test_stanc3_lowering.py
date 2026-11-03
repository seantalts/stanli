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

# stan_print's text for each value, from stan/math/prim/fun/stan_print.hpp:
# one bracketed level per extent, matrices by rows, scalars bare.
PRINTED = """print([1, 2]', " ", [[1, 2, 3], [4, 5, 6]], " ", {[1, 2]', [3, 4]'});
          print({{1, 2, 3}, {4, 5, 6}}, " ", {[[1, 2], [3, 4]], [[5, 6], [7, 8]]});
          print([7]', " ", 7, " ", {[1, 2]});"""
PRINT_TEXT = ("[1,2] [[1,2,3],[4,5,6]] [[1,2],[3,4]]\n"
              "[[1,2,3],[4,5,6]] [[[1,2],[3,4]],[[5,6],[7,8]]]\n"
              "[7] 7 [[1,2]]")

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
    ("assignment of the wrong array size",
     """parameters { array[2, 3] real x; }
        transformed parameters {
          array[2] real t = x[1, 1:2];
          t = x[1];
        }
        model { to_array_1d(x) ~ normal(0, 1); }""",
     {}, {"eval_fail": "assign array size: assigning variable t (2) and "
                       "right hand side (3) must match in size"}),
    ("declaration of the wrong vector size",
     """parameters { vector[3] v; }
        transformed parameters { vector[2] t = v; }
        model { v ~ normal(0, 1); }""",
     {}, {"eval_fail": "vector assign rows: assigning variable t (2) and "
                       "right hand side rows (3) must match in size"}),
    ("matrix assignment checks columns before rows",
     """parameters { matrix[2, 3] m; }
        transformed parameters {
          matrix[3, 2] t;
          t = m;
        }
        model { to_vector(m) ~ normal(0, 1); }""",
     {}, {"eval_fail": "matrix assign columns: assigning variable t (2) and "
                       "right hand side columns (3) must match in size"}),
    ("wrong-size assignment in generated quantities",
     """parameters { real y; }
        model { y ~ normal(0, 1); }
        generated quantities { row_vector[2] g = [y, y, y]; }""",
     {}, {"wa_fail": "vector assign columns: assigning variable g (2) and "
                     "right hand side columns (3) must match in size"}),
    ("wrong-size data assigned to an array element",
     """transformed data { array[3] real d = {1, 2, 3}; }
        parameters { real y; }
        transformed parameters {
          array[2, 2] real t;
          t[1] = d;
        }
        model { y ~ normal(0, 1); }""",
     {}, {"eval_fail": "assign array size: assigning variable t (2) and "
                       "right hand side (3) must match in size"}),
    ("wrong-size data in a declaration",
     """transformed data { vector[3] d = [1, 2, 3]'; }
        parameters { real y; }
        transformed parameters { vector[2] t = d; }
        model { y ~ normal(0, 1); }""",
     {}, {"eval_fail": "vector assign rows: assigning variable t (2) and "
                       "right hand side rows (3) must match in size"}),
    ("real divided by a matrix is elementwise",
     """parameters { real y; }
        transformed parameters {
          matrix[1, 2] t = 3 / [[2, 4]];
          matrix[1, 2] u = y / [[2, 4]];
        }
        model { y ~ normal(0, 1); }""",
     {}, {"values": [0.1, 1.5, 0.75, 0.05, 0.025]}),
    ("empty integer outcomes",
     """data { array[2, 3] int y; int a; int b; array[0] int none; }
        parameters { vector<lower=0, upper=1>[3] p; }
        model {
          y[1, a:b] ~ bernoulli(p[a:b]);
          none ~ bernoulli(p[a:b]);
          target += bernoulli_lpmf(y[1, a:b] | p[a:b]);
        }""",
     {"y": [[1, 0, 1], [0, 0, 0]], "a": 3, "b": 2, "none": []},
     {"same_as": """parameters { vector<lower=0, upper=1>[3] p; }
                    model { }"""}),
    ("fatal_error in the model block",
     """parameters { real y; }
        model {
          y ~ normal(0, 1);
          fatal_error("stopped with ", y);
        }""",
     {}, {"eval_fail": "stopped with 0.1"}),
    ("fatal_error behind a parameter branch",
     """parameters { real y; }
        model {
          y ~ normal(0, 1);
          if (y > 0) fatal_error("positive ", y);
        }""",
     {}, {"eval_fail": "positive 0.1"}),
    ('vector element index out of range',
     """data { int i; }
        parameters { vector[2] v; }
        model { v ~ normal(0, 1); target += v[i]; }""",
     {'i': 0}, {"eval_fail": 'vector[uni] indexing: accessing element out of range. index 0 out of range; expecting index to be between 1 and 2'}),
    ('array element index out of range',
     """data { int i; }
        parameters { array[2] real a; }
        model { a ~ normal(0, 1); target += a[i]; }""",
     {'i': 3}, {"eval_fail": 'array[uni, ...] index: accessing element out of range. index 3 out of range; expecting index to be between 1 and 2'}),
    ('vector multi-index out of range',
     """data { array[2] int idx; }
        parameters { vector[2] v; }
        model { v ~ normal(0, 1); target += sum(v[idx]); }""",
     {'idx': [1, 0]}, {"eval_fail": 'vector[multi] indexing: accessing element out of range. index 0 out of range; expecting index to be between 1 and 2'}),
    ('array multi-index out of range',
     """data { array[2] int idx; }
        parameters { array[2] real a; }
        model { a ~ normal(0, 1); target += sum(a[idx]); }""",
     {'idx': [2, 5]}, {"eval_fail": 'array[..., ...] index: accessing element out of range. index 5 out of range; expecting index to be between 1 and 2'}),
    ('matrix row index out of range',
     """data { int i; }
        parameters { matrix[2, 3] m; }
        model { to_vector(m) ~ normal(0, 1); target += sum(m[i]); }""",
     {'i': 3}, {"eval_fail": 'matrix[uni] indexing: accessing element out of range. index 3 out of range; expecting index to be between 1 and 2'}),
    ('matrix column index out of range with the flat offset in range',
     """data { int i; }
        parameters { matrix[3, 2] m; }
        model { to_vector(m) ~ normal(0, 1); target += m[i, 1]; }""",
     {'i': 4}, {"eval_fail": 'matrix[uni,uni] row indexing: accessing element out of range. index 4 out of range; expecting index to be between 1 and 3'}),
    ('nested array index out of range with the flat offset in range',
     """data { int j; }
        parameters { array[2, 3] real a; }
        model { to_array_1d(a) ~ normal(0, 1); target += a[1, j]; }""",
     {'j': 4}, {"eval_fail": 'array[uni, ...] index: accessing element out of range. index 4 out of range; expecting index to be between 1 and 3'}),
    ('index out of range in generated quantities',
     """data { int i; }
        parameters { vector[2] v; }
        model { v ~ normal(0, 1); }
        generated quantities { real g = v[i]; }""",
     {'i': 0}, {"wa_fail": 'vector[uni] indexing: accessing element out of range. index 0 out of range; expecting index to be between 1 and 2'}),
    ("index out of range behind a parameter branch",
     """parameters { vector[2] v; }
        model {
          v ~ normal(0, 1);
          if (v[1] > 0) target += v[0];
        }""",
     {}, {"eval_fail": "vector[uni] indexing: accessing element out of range. "
                       "index 0 out of range; expecting index to be between "
                       "1 and 2"}),
    ("index out of range in a parameter branch not taken",
     """parameters { vector[2] v; }
        model {
          v ~ normal(0, 1);
          if (v[1] > 100) target += v[3];
        }""",
     {}, {"same_as": """parameters { vector[2] v; }
                        model { v ~ normal(0, 1); }"""}),
    ("an array of complex is refused, not written as reals",
     """parameters { real r; }
        transformed parameters { array[2] complex z = {r, 3}; }
        model { r ~ normal(0, 1); }""",
     {}, {"compile_fail": "complex numbers are not supported yet"}),
    ("adding vectors of different sizes",
     """parameters { vector[2] v; }
        model { v ~ normal(0, 1); target += sum(v + [1, 2, 3]'); }""",
     {}, {"eval_fail": "add: a (2, 1) and b (3, 1) must match in size"}),
    ("subtracting data row vectors of different sizes",
     """data { row_vector[2] x; row_vector[3] y; }
        parameters { real q; }
        model { q ~ normal(0, 1); target += q * sum(x - y); }""",
     {"x": [1, 2], "y": [1, 2, 3]},
     {"eval_fail": "subtract: m1 (1, 2) and m2 (1, 3) must match in size"}),
    ("matrix column index out of range",
     """data { int j; }
        parameters { matrix[2, 3] m; }
        model { to_vector(m) ~ normal(0, 1); target += sum(m[:, j]); }""",
     {"j": 4}, {"eval_fail": "matrix[..., uni] column indexing: accessing "
                             "element out of range. index 4 out of range; "
                             "expecting index to be between 1 and 3"}),
    ("jacobian += adds to the target",
     """parameters { real y; }
        transformed parameters {
          real e = exp(y);
          jacobian += y;
        }
        model { e ~ exponential(1); }""",
     {}, {"same_as": """parameters { real<lower=0> e; }
                        model { e ~ exponential(1); }"""}),
    ("jacobian += inside an overloaded user function",
     """functions {
          real ub_jacobian(real x, real ub) {
            jacobian += x;
            return ub - exp(x);
          }
          vector ub_jacobian(vector x, real ub) {
            vector[size(x)] result;
            for (n in 1:size(x)) result[n] = ub_jacobian(x[n], ub);
            return result;
          }
        }
        parameters { vector[2] raw; }
        transformed parameters { vector[2] b = ub_jacobian(raw, 1); }
        model { b ~ normal(0, 1); }""",
     {}, {"same_as": """parameters { vector<upper=1>[2] b; }
                        model { b ~ normal(0, 1); }"""}),
    ("a complex generated quantity is refused, not written as reals",
     """parameters { real r; }
        model { r ~ normal(0, 1); }
        generated quantities { array[2] complex z = {1, 3}; complex w = 2; }""",
     {}, {"wa_fail": "complex numbers are not supported yet"}),
    ("a static index on a short-circuited operand is not evaluated",
     """data { int k; }
        parameters { vector[2] v; }
        model {
          v ~ normal(0, 1);
          if (k >= 1 && v[k] > 0) target += 1;
          if (v[1] > 100 && v[3] > 0) target += 1;
        }""",
     {"k": 0}, {"same_as": """parameters { vector[2] v; }
                              model { v ~ normal(0, 1); }"""}),
    ("a static index in the ternary arm not taken is not evaluated",
     """parameters { vector[2] v; }
        model { v ~ normal(0, 1); target += v[1] > 100 ? v[3] : v[2]; }""",
     {}, {"same_as": """parameters { vector[2] v; }
                        model { v ~ normal(0, 1); target += v[2]; }"""}),
    ("row_vector GLM rows in a loop",
     """data { int N; array[N, 2] int y; array[N] row_vector[2] xr; }
        parameters { real a; vector[2] b; }
        model {
          for (n in 1:N) target += poisson_log_glm_lpmf(y[n] | xr[n], a, b);
        }""",
     {"N": 3, "y": [[1, 0], [2, 1], [0, 3]],
      "xr": [[1, 2], [0.5, -1], [2, 0.3]]},
     {"same_as": """data { int N; array[N, 2] int y; array[N] row_vector[2] xr; }
        parameters { real a; vector[2] b; }
        model {
          for (n in 1:N)
            for (k in 1:2) target += poisson_log_lpmf(y[n, k] | a + xr[n] * b);
        }"""}),
    ("print renders containers as stan_print does, in transformed data",
     """transformed data {
          PRINTED
        }
        parameters { real y; }
        model { y ~ normal(0, 1); }""".replace("PRINTED", PRINTED),
     {}, {"stdout": PRINT_TEXT}),
    ("print renders containers as stan_print does, in the model",
     """parameters { real y; }
        model {
          y ~ normal(0, 1);
          PRINTED
        }""".replace("PRINTED", PRINTED),
     {}, {"stdout": PRINT_TEXT}),
    ("print renders containers as stan_print does, under a parameter branch",
     """parameters { real y; }
        model {
          y ~ normal(0, 1);
          if (y > 0) {
            PRINTED
          }
        }""".replace("PRINTED", PRINTED),
     {}, {"stdout": PRINT_TEXT}),
    ("reject renders a matrix by rows",
     """parameters { real y; }
        model {
          y ~ normal(0, 1);
          reject("A:", [[1, 2], [3, 4]] + y);
        }""",
     {}, {"eval_fail": "A:[[1.1,2.1],[3.1,4.1]]"}),
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
    if "same_as" in want:
        ref = run(want["same_as"], data)
        ok = "\nOK " in "\n" + out and [
            line for line in out.splitlines() if line.startswith("OK ")
        ] == [line for line in ref.splitlines() if line.startswith("OK ")]
    if "eval_fail" in want:
        ok = want["eval_fail"] in lines.get("EVAL_FAIL", "")
    if "wa_fail" in want:
        ok = want["wa_fail"] in lines.get("WANAMES", "")
    if "compile_fail" in want:
        ok = want["compile_fail"] in lines.get("COMPILE_FAIL", "")
    if "stdout" in want:
        ok = ok and want["stdout"] in out
    if not ok:
        failures += 1
        print(f"FAIL {name}\n{out}")
print(f"{len(CASES) - failures}/{len(CASES)} cases OK")
sys.exit(1 if failures else 0)
