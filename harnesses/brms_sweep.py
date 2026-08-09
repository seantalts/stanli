#!/usr/bin/env python3
"""The shapes brms generates, against CmdStan.

brms is half of Stan's user base, and its generated code is a corpus
posteriordb does not contain: `lprior` accumulation, the `scale_r_cor`
helper, per-group `r_1_1[J_1[n]] * Z_1_1[n]` indexing, spline blocks,
monotonic effects. A model that does not lower has no speedup, so this
asks the only question that matters first -- does it lower -- and then
the one that matters more: is it right.

posteriordb's `diamonds` IS real brms output (brms 2.10.0) and already
verifies, which covers the population-level-only shape. The cases here
are faithful reconstructions of the idioms brms emits for everything
else; brms itself needs R, which is not a dependency of this repo.

    harnesses/brms_sweep.py .                    # does it lower
    harnesses/brms_sweep.py . deps/cmdstan       # and is it right

Needs a CmdStan checkout for the second form, so it does not run in CI;
tests/fixtures/brmsshapes.stan is what guards this on every push.
"""

import json
import pathlib
import tempfile
import subprocess
import sys

REPO = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
CMDSTAN = pathlib.Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent
                       / "tools"))
from cmdstan_ref import build_reference, compare_points  # noqa: E402
# A temp dir, NOT the harness directory: these models generate a .stan,
# a .hpp, and a compiled reference binary each, and writing them beside
# the source is how they end up in a commit.
HERE = pathlib.Path(tempfile.mkdtemp(prefix="brms_sweep_"))

CASES = {}

# 1. Population-level only, brms >= 2.17 style with lprior.
CASES["popn_lprior"] = ("""
data {
  int<lower=1> N; vector[N] Y; int<lower=1> K; matrix[N, K] X;
  int prior_only;
}
transformed data {
  int Kc = K - 1;
  matrix[N, Kc] Xc;
  vector[Kc] means_X;
  for (i in 2:K) {
    means_X[i - 1] = mean(X[:, i]);
    Xc[:, i - 1] = X[:, i] - means_X[i - 1];
  }
}
parameters { vector[Kc] b; real Intercept; real<lower=0> sigma; }
transformed parameters {
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0, 2.5);
  lprior += student_t_lpdf(sigma | 3, 0, 2.5)
            - 1 * student_t_lccdf(0 | 3, 0, 2.5);
}
model {
  if (!prior_only) {
    target += normal_id_glm_lpdf(Y | Xc, Intercept, b, sigma);
  }
  target += lprior;
}
generated quantities {
  real b_Intercept = Intercept - dot_product(means_X, b);
}
""", {"N": 8, "K": 3, "Y": [1.0, 2.0, 1.5, 0.5, 2.5, 1.1, 0.9, 1.8],
      "X": [[1, 0.2, 0.5]] * 8, "prior_only": 0})

# 2. Multilevel, uncorrelated random intercepts -- the (1|g) shape.
CASES["mlm_uncorrelated"] = ("""
data {
  int<lower=1> N; vector[N] Y; int<lower=1> K; matrix[N, K] X;
  int<lower=1> N_1; int<lower=1> M_1; array[N] int<lower=1> J_1;
  vector[N] Z_1_1; int prior_only;
}
transformed data {
  int Kc = K - 1;
  matrix[N, Kc] Xc;
  vector[Kc] means_X;
  for (i in 2:K) {
    means_X[i - 1] = mean(X[:, i]);
    Xc[:, i - 1] = X[:, i] - means_X[i - 1];
  }
}
parameters {
  vector[Kc] b; real Intercept; real<lower=0> sigma;
  vector<lower=0>[M_1] sd_1; array[M_1] vector[N_1] z_1;
}
transformed parameters {
  vector[N_1] r_1_1 = (sd_1[1] * (z_1[1]));
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0, 2.5);
  lprior += student_t_lpdf(sigma | 3, 0, 2.5)
            - 1 * student_t_lccdf(0 | 3, 0, 2.5);
  lprior += student_t_lpdf(sd_1 | 3, 0, 2.5)
            - 1 * student_t_lccdf(0 | 3, 0, 2.5);
}
model {
  if (!prior_only) {
    vector[N] mu = rep_vector(0.0, N);
    mu += Intercept + Xc * b;
    for (n in 1:N) {
      mu[n] += r_1_1[J_1[n]] * Z_1_1[n];
    }
    target += normal_lpdf(Y | mu, sigma);
  }
  target += lprior;
  target += std_normal_lpdf(z_1[1]);
}
""", {"N": 8, "K": 3, "Y": [1.0, 2.0, 1.5, 0.5, 2.5, 1.1, 0.9, 1.8],
      "X": [[1, 0.2, 0.5]] * 8, "N_1": 3, "M_1": 1,
      "J_1": [1, 1, 2, 2, 3, 3, 1, 2], "Z_1_1": [1.0] * 8,
      "prior_only": 0})

# 3. Multilevel, CORRELATED random effects -- (1+x|g). This is the shape
# with brms's scale_r_cor helper and a cholesky_factor_corr parameter.
CASES["mlm_correlated"] = ("""
functions {
  matrix scale_r_cor(matrix z, vector SD, matrix L) {
    return transpose(diag_pre_multiply(SD, L) * z);
  }
}
data {
  int<lower=1> N; vector[N] Y;
  int<lower=1> N_1; int<lower=1> M_1; array[N] int<lower=1> J_1;
  vector[N] Z_1_1; vector[N] Z_1_2; int<lower=1> NC_1;
  int prior_only;
}
parameters {
  real Intercept; real<lower=0> sigma;
  vector<lower=0>[M_1] sd_1; matrix[M_1, N_1] z_1;
  cholesky_factor_corr[M_1] L_1;
}
transformed parameters {
  matrix[N_1, M_1] r_1 = scale_r_cor(z_1, sd_1, L_1);
  vector[N_1] r_1_1 = r_1[:, 1];
  vector[N_1] r_1_2 = r_1[:, 2];
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0, 2.5);
  lprior += student_t_lpdf(sigma | 3, 0, 2.5)
            - 1 * student_t_lccdf(0 | 3, 0, 2.5);
  lprior += student_t_lpdf(sd_1 | 3, 0, 2.5)
            - 2 * student_t_lccdf(0 | 3, 0, 2.5);
  lprior += lkj_corr_cholesky_lpdf(L_1 | 1);
}
model {
  if (!prior_only) {
    vector[N] mu = rep_vector(0.0, N);
    mu += Intercept;
    for (n in 1:N) {
      mu[n] += r_1_1[J_1[n]] * Z_1_1[n] + r_1_2[J_1[n]] * Z_1_2[n];
    }
    target += normal_lpdf(Y | mu, sigma);
  }
  target += lprior;
  target += std_normal_lpdf(to_vector(z_1));
}
generated quantities {
  corr_matrix[M_1] Cor_1 = multiply_lower_tri_self_transpose(L_1);
}
""", {"N": 8, "Y": [1.0, 2.0, 1.5, 0.5, 2.5, 1.1, 0.9, 1.8],
      "N_1": 3, "M_1": 2, "J_1": [1, 1, 2, 2, 3, 3, 1, 2],
      "Z_1_1": [1.0] * 8, "Z_1_2": [0.4] * 8, "NC_1": 1, "prior_only": 0})

# 4. Bernoulli family, the logistic-regression default.
CASES["bernoulli_glm"] = ("""
data {
  int<lower=1> N; array[N] int Y; int<lower=1> K; matrix[N, K] X;
  int prior_only;
}
transformed data {
  int Kc = K - 1;
  matrix[N, Kc] Xc;
  vector[Kc] means_X;
  for (i in 2:K) {
    means_X[i - 1] = mean(X[:, i]);
    Xc[:, i - 1] = X[:, i] - means_X[i - 1];
  }
}
parameters { vector[Kc] b; real Intercept; }
transformed parameters {
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0, 2.5);
}
model {
  if (!prior_only) {
    target += bernoulli_logit_glm_lpmf(Y | Xc, Intercept, b);
  }
  target += lprior;
}
""", {"N": 8, "K": 3, "Y": [0, 1, 1, 0, 1, 1, 0, 1],
      "X": [[1, 0.2, 0.5]] * 8, "prior_only": 0})

# 5. Ordinal (cumulative family): ordered thresholds + the ordinal GLM.
CASES["ordinal_cumulative"] = ("""
data {
  int<lower=1> N; array[N] int Y; int<lower=2> nthres;
  int<lower=1> K; matrix[N, K] X; int prior_only;
}
transformed data {
  int Kc = K;
  matrix[N, Kc] Xc;
  for (i in 1:K) { Xc[:, i] = X[:, i]; }
}
parameters { vector[Kc] b; ordered[nthres] Intercept; }
transformed parameters {
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0, 2.5);
}
model {
  if (!prior_only) {
    target += ordered_logistic_glm_lpmf(Y | Xc, b, Intercept);
  }
  target += lprior;
}
""", {"N": 8, "K": 2, "nthres": 2, "Y": [1, 2, 3, 1, 2, 3, 1, 2],
      "X": [[0.3, 0.5]] * 8, "prior_only": 0})

# 6. Smooth terms, the s() shape: a spline block with its own sd.
CASES["splines"] = ("""
data {
  int<lower=1> N; vector[N] Y; int<lower=1> Ks; matrix[N, Ks] Xs;
  int<lower=1> knots_1; matrix[N, knots_1] Zs_1_1; int prior_only;
}
parameters {
  real Intercept; real<lower=0> sigma; vector[Ks] bs;
  vector[knots_1] zs_1_1; real<lower=0> sds_1_1;
}
transformed parameters {
  vector[knots_1] s_1_1 = sds_1_1 * zs_1_1;
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0, 2.5);
  lprior += student_t_lpdf(sds_1_1 | 3, 0, 2.5)
            - 1 * student_t_lccdf(0 | 3, 0, 2.5);
}
model {
  if (!prior_only) {
    vector[N] mu = rep_vector(0.0, N);
    mu += Intercept + Xs * bs + Zs_1_1 * s_1_1;
    target += normal_lpdf(Y | mu, sigma);
  }
  target += lprior;
  target += std_normal_lpdf(zs_1_1);
}
""", {"N": 8, "Y": [1.0, 2.0, 1.5, 0.5, 2.5, 1.1, 0.9, 1.8],
      "Ks": 1, "Xs": [[0.5]] * 8, "knots_1": 2,
      "Zs_1_1": [[0.2, 0.3]] * 8, "prior_only": 0})

# 7. Monotonic effects, mo(): a simplex per monotonic predictor plus
# brms's mo() helper, which indexes with a runtime bound.
CASES["monotonic"] = ("""
functions {
  real mo(vector scale, int i) {
    if (i == 0) {
      return 0;
    } else {
      return rows(scale) * sum(scale[1:i]);
    }
  }
}
data {
  int<lower=1> N; vector[N] Y; int<lower=1> Imo;
  array[N] int Xmo_1; int<lower=1> Jmo_1; int prior_only;
}
parameters {
  real Intercept; real<lower=0> sigma; real bsp_1;
  simplex[Jmo_1] simo_1;
}
transformed parameters {
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0, 2.5);
  lprior += dirichlet_lpdf(simo_1 | rep_vector(1.0, Jmo_1));
}
model {
  if (!prior_only) {
    vector[N] mu = rep_vector(0.0, N);
    for (n in 1:N) {
      mu[n] += Intercept + bsp_1 * mo(simo_1, Xmo_1[n]);
    }
    target += normal_lpdf(Y | mu, sigma);
  }
  target += lprior;
}
""", {"N": 8, "Y": [1.0, 2.0, 1.5, 0.5, 2.5, 1.1, 0.9, 1.8],
      "Imo": 1, "Xmo_1": [0, 1, 2, 1, 2, 0, 1, 2], "Jmo_1": 2,
      "prior_only": 0})

# 8. Posterior predictive in generated quantities -- what every brms fit
# needs for pp_check().
CASES["gq_predictions"] = ("""
data { int<lower=1> N; vector[N] Y; int prior_only; }
parameters { real Intercept; real<lower=0> sigma; }
transformed parameters {
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0, 2.5);
}
model {
  if (!prior_only) { target += normal_lpdf(Y | Intercept, sigma); }
  target += lprior;
}
generated quantities {
  real b_Intercept = Intercept;
  array[N] real yrep;
  vector[N] log_lik;
  for (n in 1:N) {
    yrep[n] = normal_rng(Intercept, sigma);
    log_lik[n] = normal_lpdf(Y[n] | Intercept, sigma);
  }
}
""", {"N": 8, "Y": [1.0, 2.0, 1.5, 0.5, 2.5, 1.1, 0.9, 1.8],
      "prior_only": 0})


MAX_REL = 1e-11
ABS_FLOOR = 1e-13


def reldiff(a, b):
    d = abs(a - b)
    if d == 0:
        return 0.0
    scale = max(abs(a), abs(b))
    if scale < ABS_FLOOR:
        return 0.0 if d < ABS_FLOOR else d
    return d / scale


def verify_one(cs, d, name, check, stanc):
    """lp and every gradient component, at three deterministic points."""
    stan, data = d / "m.stan", d / "d.json"
    exe, err = build_reference(cs, d, stan, REPO / "tools/ref_driver.cpp",
                               stanc, name=name, sundials=False)
    if exe is None:
        return err
    worst, n_cmp, err = compare_points(exe, check, stan, data, reldiff, stanc)
    if err:
        kind, detail = err
        return (f"one side threw at {detail}" if kind == "one_side_threw"
                else f"shape mismatch: {detail}")
    if n_cmp == 0:
        return "no valid point"
    if worst > MAX_REL:
        return f"{n_cmp} values, {worst:.2e} rel"
    return None if worst else None


def main():
    check = REPO / "build" / "stanli_check"
    stanc = REPO / "deps" / "stanc3" / "stanc"
    ok = 0
    for name, (src, data) in CASES.items():
        d = HERE / name
        d.mkdir(exist_ok=True)
        (d / "m.stan").write_text(src)
        (d / "d.json").write_text(json.dumps(data))
        r = subprocess.run(
            [str(check), str(d / "m.stan"), str(d / "d.json"),
             "--stanc", str(stanc)],
            capture_output=True, text=True)
        out = r.stdout + r.stderr
        if "COMPILE_FAIL" in out:
            why = [l for l in out.strip().splitlines()
                   if "COMPILE_FAIL" in l][0]
            print(f"FAIL  {name:22s} {why[:100]}")
        elif "EVAL_FAIL" in out:
            why = [l for l in out.strip().splitlines()
                   if "EVAL_FAIL" in l][0]
            print(f"EVAL  {name:22s} {why[:100]}")
        elif "OK" in out:
            if CMDSTAN is None:
                wa = [l for l in out.splitlines() if l.startswith("WA")]
                print(f"ok    {name:22s} lowers and evaluates"
                      f"{'  |  ' + wa[0] if wa else ''}")
                ok += 1
                continue
            # Lowering is not the bar. A model that compiles and returns
            # the wrong gradient is worse than one that refuses.
            bad = verify_one(CMDSTAN, d, name, check, stanc)
            if bad:
                print(f"FAIL  {name:22s} {bad}")
            else:
                print(f"ok    {name:22s} verified vs CmdStan")
                ok += 1
        else:
            print(f"?     {name:22s} {out.strip()[:100]}")
    what = "verified vs CmdStan" if CMDSTAN else "lower and evaluate"
    print(f"\n{ok}/{len(CASES)} brms-shaped models {what}")
    print(f"artifacts in {HERE}")


if __name__ == "__main__":
    main()
