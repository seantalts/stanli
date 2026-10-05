#!/usr/bin/env python3
"""Write one small model per density, each with and without the density.

  density_models.py OUT_DIR deps/stanc3/stanc

For every density D, OUT_DIR/D and OUT_DIR/D_base hold model.stan,
data.json and model.tmir.sexp (stanc --O1 --debug-optimized-mir). The two
differ only in the one vectorized statement over N=64 observations whose
arguments are a parameter-derived vector and parameter scalars, so the
difference in allocations per gradient between them belongs to D.
"""
import json
import math
import pathlib
import subprocess
import sys

N = 64
DATA = """
data { int N; vector[N] x; vector[N] yc; vector<lower=0>[N] yp;
       vector<lower=0, upper=1>[N] yu; array[N] int yb; array[N] int yn;
       array[N] int<lower=1> nt; array[N] int<lower=1, upper=4> yo; }
"""
PARAMS = """
parameters { real a; real b; real<lower=0> s; real<lower=1> nu;
             ordered[3] c; vector[N] z; }
"""
PRIORS = """
  a ~ normal(0, 1); b ~ normal(0, 1); s ~ normal(0, 1);
  nu ~ gamma(2, 0.1); c ~ normal(0, 3);
"""
STATEMENTS = {
    "student_t": "yc ~ student_t(nu, mu, s);",
    "lognormal": "yp ~ lognormal(mu, s);",
    "exponential": "yp ~ exponential(s);",
    "gamma": "yp ~ gamma(nu, s);",
    "beta": "yu ~ beta(nu, s + 1);",
    "bernoulli_logit": "yb ~ bernoulli_logit(mu);",
    "poisson_log": "yn ~ poisson_log(mu);",
    "binomial_logit": "yn ~ binomial_logit(nt, mu);",
    "ordered_logistic": "yo ~ ordered_logistic(mu, c);",
    "std_normal": "z ~ std_normal();",
    "poisson": "yn ~ poisson(exp(mu));",
    "bernoulli": "yb ~ bernoulli(inv_logit(mu));",
    "binomial": "yn ~ binomial(nt, inv_logit(mu));",
    "normal": "yc ~ normal(mu, s);",
    "cauchy": "yc ~ cauchy(mu, s);",
}


def model(statement):
    return (DATA + PARAMS + "model {\n  vector[N] mu = a + b * x;" + PRIORS +
            (("  " + statement + "\n") if statement else "") + "}\n")


def data():
    x = [math.sin(0.37 * i) for i in range(N)]
    nt = [5 + i % 7 for i in range(N)]
    return {"N": N, "x": x,
            "yc": [0.3 * v + 0.1 * math.cos(1.9 * i) for i, v in enumerate(x)],
            "yp": [0.5 + abs(math.sin(0.7 * i)) for i in range(N)],
            "yu": [0.1 + 0.8 * abs(math.sin(0.43 * i)) for i in range(N)],
            "yb": [i % 2 for i in range(N)],
            "yn": [i % 4 for i in range(N)],
            "nt": nt,
            "yo": [1 + i % 4 for i in range(N)]}


def write(directory, statement, stanc):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "model.stan").write_text(model(statement))
    (directory / "data.json").write_text(json.dumps(data()))
    mir = subprocess.run([stanc, "--O1", "--debug-optimized-mir", "model.stan"],
                         cwd=directory, capture_output=True, text=True,
                         check=True).stdout
    (directory / "model.tmir.sexp").write_text(mir)
    (directory / "model.hpp").unlink(missing_ok=True)


def main():
    out = pathlib.Path(sys.argv[1])
    stanc = str(pathlib.Path(sys.argv[2]).resolve())
    write(out / "base", "", stanc)
    for name, statement in STATEMENTS.items():
        write(out / name, statement, stanc)


if __name__ == "__main__":
    main()
