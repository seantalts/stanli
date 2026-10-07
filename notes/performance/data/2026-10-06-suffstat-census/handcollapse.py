#!/usr/bin/env python3
"""Measured spot check of two ceiling estimates with hand-collapsed models.

radon_county   shape (a): per-county (n, ybar, S) instead of 12573 terms.
ch12_m12_5     shape (b): 42 distinct (R, A, C, I) rows with counts.
Writes the collapsed Stan + data under scratch/suffstat/hand/, compiles the
MIR with the pinned probe and times both versions with bench_grad (shipped
configuration, 3 alternating rounds, minimum).  Wall-clock, noisy.
"""
import collections
import json
import pathlib
import subprocess
from fractions import Fraction

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
PROBE = REPO / "deps/stanc3/stanli-vectorize-probe"
BENCH = REPO / "build-spike/bench_grad"
HAND = HERE / "hand"
HAND.mkdir(exist_ok=True)
status = {s["name"]: s for s in json.loads((HERE / "stage1_status.json").read_text())}

RADON = """
data { int<lower=0> J; vector[J] n; vector[J] ybar; vector[J] S; }
parameters {
  vector[J] a; real mu_a;
  real<lower=0, upper=100> sigma_a; real<lower=0, upper=100> sigma_y;
}
model {
  mu_a ~ normal(0, 1);
  a ~ normal(mu_a, sigma_a);
  target += -sum(n) * log(sigma_y)
            - (sum(S) + dot_product(n, square(ybar - a))) / (2 * square(sigma_y));
}
"""
ORD = """
data { int K; array[K] int R; vector[K] A; vector[K] C; vector[K] I; vector[K] w; }
parameters { real bIC; real bIA; real bC; real bI; real bA; ordered[6] cutpoints; }
model {
  vector[K] phi; vector[K] BI;
  cutpoints ~ normal(0, 1.5);
  bA ~ normal(0, 0.5); bI ~ normal(0, 0.5); bC ~ normal(0, 0.5);
  bIA ~ normal(0, 0.5); bIC ~ normal(0, 0.5);
  BI = bI + bIA * A + bIC * C;
  phi = bA * A + bC * C + BI .* I;
  for (k in 1:K) target += w[k] * ordered_logistic_lpmf(R[k] | phi[k], cutpoints);
}
"""


def build(name, stan, data):
    d = HAND / name
    d.mkdir(exist_ok=True)
    (d / "model.stan").write_text(stan)
    (d / "data.json").write_text(json.dumps(data))
    subprocess.run([str(PROBE), "--vectorize-loops", "on", "--output",
                    str(d / "model.sexp"), str(d / "model.stan")], check=True)
    return d / "model.sexp", d / "data.json"


def ns(mir, data, n):
    r = subprocess.run([str(BENCH), str(mir), str(data), str(n)],
                       capture_output=True, text=True, check=True)
    return float(r.stdout.split()[0])


def compare(name, orig_data, mir2, data2):
    mir1 = HERE / "out" / name / "model.sexp"
    a, b = [], []
    for _ in range(3):
        a.append(ns(mir1, orig_data, 3000))
        b.append(ns(mir2, data2, 30000))
    est = {r["name"]: r for r in json.loads(
        (HERE / "analysis_with_estimates.json").read_text())}[name]
    print(f"{name}: original {min(a):.0f} ns/grad, hand-collapsed {min(b):.0f} "
          f"ns/grad, measured {min(a) / min(b):.1f}x; estimated ceiling "
          f"{est['vs_default_best']:.1f}x")


d = json.loads(pathlib.Path(status["radon_county"]["data"]).read_text())
g = collections.defaultdict(list)
for y, c in zip(d["y"], d["county"]):
    g[c].append(Fraction(y))
n, ybar, S = [], [], []
for j in range(1, d["J"] + 1):
    ys = g[j]
    m = float(sum(ys) / len(ys))
    n.append(float(len(ys)))
    ybar.append(m)
    S.append(float(sum((v - Fraction(m)) ** 2 for v in ys)))
compare("radon_county", status["radon_county"]["data"],
        *build("radon_county_suffstat", RADON, dict(J=d["J"], n=n, ybar=ybar, S=S)))

d = json.loads((REPO / "tests/rethinking/ch12_m12_5.json").read_text())
cnt = collections.Counter(zip(d["R"], d["A"], d["C"], d["I"]))
rows = sorted(cnt)
compare("ch12_m12_5", REPO / "tests/rethinking/ch12_m12_5.json",
        *build("ch12_m12_5_weighted", ORD, dict(
            K=len(rows), R=[r[0] for r in rows], A=[float(r[1]) for r in rows],
            C=[float(r[2]) for r in rows], I=[float(r[3]) for r in rows],
            w=[float(cnt[r]) for r in rows])))
