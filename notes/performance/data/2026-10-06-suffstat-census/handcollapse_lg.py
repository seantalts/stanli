#!/usr/bin/env python3
"""Measured spot check: diamonds with the least-squares-centred triangular form.

Writes scratch/suffstat/hand/diamonds_lsc/, times it against the original
with bench_grad (shipped configuration, 3 alternating rounds, minimum) and
compares gradients at the three corpus points through stanli_check.
Preparation here is plain double precision (numpy), good enough for timing.
"""
import json
import math
import pathlib
import subprocess

import numpy as np

import lg_numerics as L

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
PROBE = REPO / "deps/stanc3/stanli-vectorize-probe"
BENCH = REPO / "build-spike/bench_grad"
CHECK = REPO / "build-spike/stanli_check"

STAN = """
data { int P; int N; matrix[P, P] Rz; vector[P] th_hat; vector[P] d; real rss; }
parameters { vector[P - 1] b; real Intercept; real<lower=0> sigma; }
model {
  target += normal_lpdf(b | 0, 1);
  target += student_t_lpdf(Intercept | 3, 8, 10);
  target += student_t_lpdf(sigma | 3, 0, 10) - 1 * student_t_lccdf(0 | 3, 0, 10);
  {
    vector[P] dl = append_row(b, Intercept) - th_hat;
    vector[P] w = Rz * dl;
    target += -N * log(sigma)
              - (rss - 2 * dot_product(dl, d) + dot_self(w)) / (2 * square(sigma));
  }
}
"""

Z, y, c = L.design("diamonds")
yt = y - c
N, P = Z.shape
th = np.linalg.lstsq(Z, yt, rcond=None)[0]
res = yt - Z @ th
hand = HERE / "hand" / "diamonds_lsc"
hand.mkdir(parents=True, exist_ok=True)
(hand / "model.stan").write_text(STAN)
(hand / "data.json").write_text(json.dumps(dict(
    P=P, N=N, Rz=np.linalg.qr(Z, mode="r").tolist(), th_hat=th.tolist(),
    d=(Z.T @ res).tolist(), rss=float(res @ res))))
subprocess.run([str(PROBE), "--vectorize-loops", "on", "--output",
                str(hand / "model.sexp"), str(hand / "model.stan")], check=True)
orig = HERE / "out" / "diamonds"
odata = L.status["diamonds"]["data"]


def ns(mir, data, n):
    r = subprocess.run([str(BENCH), str(mir), str(data), str(n)],
                       capture_output=True, text=True, check=True)
    return float(r.stdout.split()[0])


def grad(d, data, pt):
    out = subprocess.run([str(CHECK), str(d / "model.stan"), str(data), "--mir",
                          str(d / "model.sexp"), "--point", str(pt)],
                         capture_output=True, text=True).stdout
    return [float(x) for x in [ln for ln in out.splitlines()
                               if ln.startswith("OK")][-1].split()[2:]]


a, b = [], []
for _ in range(3):
    a.append(ns(orig / "model.sexp", odata, 5000))
    b.append(ns(hand / "model.sexp", hand / "data.json", 50000))
est = [r for r in json.loads((HERE / "analysis2.json").read_text())
       if r["name"] == "diamonds"][0]
print(f"diamonds (N={N}, P={P}): original {min(a):.0f} ns/grad, hand-collapsed "
      f"{min(b):.0f} ns/grad, measured {min(a) / min(b):.1f}x")
for pt in (0, 1, 2):
    g0, g1 = grad(orig, odata, pt), grad(hand, hand / "data.json", pt)
    big = max(abs(v) for v in g0)
    unit = math.nextafter(big, math.inf) - big
    print(f"  pt{pt}: max|g|={big:.4g}, max abs diff "
          f"{max(abs(x - y) for x, y in zip(g0, g1)) / unit:.1f} ULP of the largest entry")
