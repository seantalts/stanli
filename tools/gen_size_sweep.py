#!/usr/bin/env python3
"""Writes data for a size sweep: the same model at 10^3, 10^4 and 10^5 rows.

The corpus records every model at one size, so a preparation step that grows
faster than the data stays invisible there. This script writes data files for
four models, with a fixed seed, at the three sizes. The large files are not
checked in and are not part of the corpus or CI; generate them where you
need them:

    python3 tools/gen_size_sweep.py /tmp/size-sweep
    for size in 1000 10000 100000; do
      for model in linear hierarchical logistic_glm logistic_loop; do
        /usr/bin/time -p build-rel/stanli_check \\
          /tmp/size-sweep/$model.stan /tmp/size-sweep/${model}_$size.json \\
          --point 0
      done
    done

`stanli_check` compiles the model, prepares it and evaluates the log density
and gradient once, so the wall time is dominated by preparation. For the
split, set STANLI_PROFILE_PREP=1 (a developer switch, so the build needs
STANLI_DEBUG_SWITCHES=ON). Compare the time at 10^4 and 10^5 rows: a fixed
cost hides growth at 10^3.

Models (the first three are existing corpus models, copied unchanged):
  linear          posteriordb earn_height, a linear regression with a
                  vectorized likelihood
  hierarchical    posteriordb radon_variable_intercept_centered, a varying
                  intercept model with a likelihood written as a loop
  logistic_glm    posteriordb nes_logit_model, a logistic regression through
                  bernoulli_logit_glm
  logistic_loop   a logistic regression with the likelihood written as a loop
                  over rows, which no corpus model does. The program is the
                  LOGISTIC_LOOP string below.

The first three .stan files are copied from deps/posteriordb. Pass
--posteriordb to read them from another checkout.
"""
import argparse
import json
import math
import pathlib
import random
import shutil
import sys

SIZES = (1000, 10000, 100000)
SEED = 20261120
REPO = pathlib.Path(__file__).resolve().parent.parent

LOGISTIC_LOOP = """\
data {
  int<lower=0> N;
  int<lower=0> K;
  matrix[N, K] x;
  array[N] int<lower=0, upper=1> y;
}
parameters {
  real alpha;
  vector[K] beta;
}
model {
  alpha ~ normal(0, 2.5);
  beta ~ normal(0, 2.5);
  for (n in 1:N) {
    y[n] ~ bernoulli_logit(alpha + dot_product(x[n], beta));
  }
}
"""

PDB_MODELS = {"linear": "earn_height", "hierarchical": "radon_variable_intercept_centered",
              "logistic_glm": "nes_logit_model"}


def inv_logit(v):
    return 1.0 / (1.0 + math.exp(-v))


def linear(rng, n):
    height = [rng.gauss(66, 4) for _ in range(n)]
    earn = [-60000 + 1500 * h + rng.gauss(0, 20000) for h in height]
    return {"N": n, "earn": earn, "height": height}


def hierarchical(rng, n):
    groups = max(10, round(n ** 0.5 / 2))
    alpha = [rng.gauss(1.5, 0.3) for _ in range(groups)]
    county = [rng.randrange(1, groups + 1) for _ in range(n)]
    floor = [float(rng.random() < 0.3) for _ in range(n)]
    radon = [alpha[c - 1] - 0.6 * f + rng.gauss(0, 0.8) for c, f in zip(county, floor)]
    return {"J": groups, "N": n, "county_idx": county, "floor_measure": floor,
            "log_radon": radon}


def logistic_glm(rng, n):
    income = [rng.gauss(0, 1) for _ in range(n)]
    vote = [int(rng.random() < inv_logit(-0.2 + 0.8 * i)) for i in income]
    return {"N": n, "income": income, "vote": vote}


def logistic_loop(rng, n):
    k = 3
    x = [[rng.gauss(0, 1) for _ in range(k)] for _ in range(n)]
    beta = (0.8, -0.5, 0.3)
    y = [int(rng.random() < inv_logit(-0.2 + sum(b * v for b, v in zip(beta, row))))
         for row in x]
    return {"N": n, "K": k, "x": x, "y": y}


GENERATORS = {"linear": linear, "hierarchical": hierarchical,
              "logistic_glm": logistic_glm, "logistic_loop": logistic_loop}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("outdir", type=pathlib.Path)
    parser.add_argument("--posteriordb", type=pathlib.Path, default=REPO / "deps/posteriordb")
    parser.add_argument("--sizes", type=int, nargs="+", default=SIZES)
    args = parser.parse_args()
    models = args.posteriordb / "posterior_database" / "models" / "stan"
    args.outdir.mkdir(parents=True, exist_ok=True)
    for name, source in PDB_MODELS.items():
        shutil.copyfile(models / f"{source}.stan", args.outdir / f"{name}.stan")
    (args.outdir / "logistic_loop.stan").write_text(LOGISTIC_LOOP)
    for name, generate in GENERATORS.items():
        for n in args.sizes:
            rng = random.Random(f"{SEED}-{name}-{n}")
            path = args.outdir / f"{name}_{n}.json"
            path.write_text(json.dumps(generate(rng, n), separators=(",", ":")) + "\n")
            print(f"{path} {path.stat().st_size / 1e6:.1f} MB")


if __name__ == "__main__":
    sys.exit(main())
