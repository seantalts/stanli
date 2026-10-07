#!/usr/bin/env python3
"""High-precision check of the centred sufficient-statistic normal likelihood.

Throwaway spike script (run with scratch/suffstat/.venv/bin/python).  For
four corpus models whose likelihood is y ~ normal(mu_group, sigma):

  hp        70-digit mpmath reference, analytic gradient, per observation
  stanli    build-spike/stanli_check --mir (the current path), default and
            runtime fast-math
  centred   float64, per group (n, ybar, S = sum (y - ybar)^2), statistics
            prepared once in exact rational arithmetic and rounded
  centred+d the same plus the stored rounding residual d = sum (y - ybar)
  raw       float64, per group (n, sum y, sum y^2)   [the naive form]

Error unit: ULP of the largest-magnitude reference gradient entry.
"""
import json
import math
import pathlib
import subprocess
from fractions import Fraction

from mpmath import mp, mpf, exp, log, pi

mp.dps = 70
HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
CHECK = REPO / "build-spike/stanli_check"


def eval_point(i, variant):
    if variant == 1:
        return 0.02 * ((i % 5) - 2)
    if variant == 2:
        return 0.0
    return 0.1 + 0.05 * (i % 7) - 0.15 * (i % 3)


def stanli(model, data, pt, fast):
    cmd = [str(CHECK), str(HERE / "out" / model / "model.stan"), str(data),
           "--mir", str(HERE / "out" / model / "model.sexp"), "--point", str(pt)]
    if fast:
        cmd.append("--fast-math")
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    line = [ln for ln in out.splitlines() if ln.startswith("OK")]
    if not line:
        raise RuntimeError(out[-300:])
    return [float(x) for x in line[-1].split()[1:]]


def stats(y, key):
    """Exact per-group statistics, rounded once to double."""
    groups = {}
    for yi, k in zip(y, key):
        groups.setdefault(k, []).append(yi)
    res = []
    for k in sorted(groups):
        ys = [Fraction(v) for v in groups[k]]
        n = len(ys)
        ybar = float(sum(ys) / n)
        fb = Fraction(ybar)
        S = float(sum((v - fb) ** 2 for v in ys))
        d = float(sum(v - fb for v in ys))
        res.append(dict(key=k, n=n, ybar=ybar, S=S, d=d,
                        sy=float(sum(ys)), syy=float(sum(v * v for v in ys))))
    return res


def lik_centred(st, mu, sigma, use_d):
    """float64: returns (dmu per group, dsigma)."""
    inv2 = 1.0 / (sigma * sigma)
    inv3 = inv2 / sigma
    dmu, dsig = [], 0.0
    for g, m in zip(st, mu):
        r = g["ybar"] - m
        q = g["S"] + g["n"] * r * r
        s1 = g["n"] * r
        if use_d:
            s1 += g["d"]
            q += 2.0 * r * g["d"]
        dmu.append(s1 * inv2)
        dsig += -g["n"] / sigma + q * inv3
    return dmu, dsig


def lik_raw(st, mu, sigma):
    inv2 = 1.0 / (sigma * sigma)
    inv3 = inv2 / sigma
    dmu, dsig = [], 0.0
    for g, m in zip(st, mu):
        q = g["syy"] - 2.0 * m * g["sy"] + g["n"] * m * m
        dmu.append((g["sy"] - g["n"] * m) * inv2)
        dsig += -g["n"] / sigma + q * inv3
    return dmu, dsig


def lik_hp(y, mu_of, sigma):
    """mpmath, per observation: (list of (y - mu)/sigma^2, dsigma)."""
    dmu, dsig = [], mpf(0)
    for i, yi in enumerate(y):
        r = mpf(yi) - mu_of(i)
        dmu.append(r / sigma ** 2)
        dsig += -1 / sigma + r ** 2 / sigma ** 3
    return dmu, dsig


def il(u):
    return 1 / (1 + exp(-u))


def fil(u):
    return 1.0 / (1.0 + math.exp(-u))


# Each model returns the gradient in unconstrained space for arithmetic `A`
# ("hp" or one of the float64 variants).
def radon_pooled(d, u, variant):
    y, x = d["log_radon"], d["floor_measure"]
    if variant == "hp":
        a, b, us = (mpf(v) for v in u)
        s = exp(us)
        dmu, dsig = lik_hp(y, lambda i: a + b * mpf(x[i]), s)
        return [-a / 100 + sum(dmu), -b / 100 + sum(mpf(xi) * t for xi, t in zip(x, dmu)),
                (-s + dsig) * s + 1]
    a, b, us = u
    s = math.exp(us)
    st = d.setdefault("_st", stats(y, x))
    mu = [a + b * g["key"] for g in st]
    dmu, dsig = (lik_raw(st, mu, s) if variant == "raw"
                 else lik_centred(st, mu, s, variant == "centred+d"))
    ga = gb = 0.0
    for g, t in zip(st, dmu):
        ga += t
        gb += g["key"] * t
    return [-a / 100.0 + ga, -b / 100.0 + gb, (-s + dsig) * s + 1.0]


def earn_height(d, u, variant):
    y, x = d["earn"], d["height"]
    if variant == "hp":
        a, b, us = (mpf(v) for v in u)
        s = exp(us)
        dmu, dsig = lik_hp(y, lambda i: a + b * mpf(x[i]), s)
        return [sum(dmu), sum(mpf(xi) * t for xi, t in zip(x, dmu)), dsig * s + 1]
    a, b, us = u
    s = math.exp(us)
    st = d.setdefault("_st", stats(y, x))
    mu = [a + b * g["key"] for g in st]
    dmu, dsig = (lik_raw(st, mu, s) if variant == "raw"
                 else lik_centred(st, mu, s, variant == "centred+d"))
    ga = gb = 0.0
    for g, t in zip(st, dmu):
        ga += t
        gb += g["key"] * t
    return [ga, gb, dsig * s + 1.0]


def ch09_m9_4(d, u, variant):
    y = d["y"]
    if variant == "hp":
        a1, a2, us = (mpf(v) for v in u)
        s = exp(us)
        dmu, dsig = lik_hp(y, lambda i: a1 + a2, s)
        t = sum(dmu)
        return [-a1 / 10 ** 6 + t, -a2 / 10 ** 6 + t, (-1 + dsig) * s + 1]
    a1, a2, us = u
    s = math.exp(us)
    st = d.setdefault("_st", stats(y, [0] * len(y)))
    dmu, dsig = (lik_raw(st, [a1 + a2], s) if variant == "raw"
                 else lik_centred(st, [a1 + a2], s, variant == "centred+d"))
    return [-a1 / 1e6 + dmu[0], -a2 / 1e6 + dmu[0], (-1.0 + dsig) * s + 1.0]


def radon_county(d, u, variant):
    y, c, J = d["y"], d["county"], d["J"]
    hp = variant == "hp"
    F = mpf if hp else float
    a = [F(v) for v in u[:J]]
    mua, usa, usy = F(u[J]), F(u[J + 1]), F(u[J + 2])
    ia, iy = (il(usa), il(usy)) if hp else (fil(usa), fil(usy))
    sa, sy = 100 * ia, 100 * iy
    g = [-(aj - mua) / sa ** 2 for aj in a]
    gmu = -mua
    dsa = F(0)
    for aj in a:
        gmu += (aj - mua) / sa ** 2
        dsa += -1 / sa + (aj - mua) ** 2 / sa ** 3
    if hp:
        dmu, dsy = lik_hp(y, lambda i: a[c[i] - 1], sy)
        for i, t in enumerate(dmu):
            g[c[i] - 1] += t
    else:
        st = d.setdefault("_st", stats(y, c))
        mu = [a[gr["key"] - 1] for gr in st]
        dmu, dsy = (lik_raw(st, mu, sy) if variant == "raw"
                    else lik_centred(st, mu, sy, variant == "centred+d"))
        for gr, t in zip(st, dmu):
            g[gr["key"] - 1] += t
    return g + [gmu, dsa * sa * (1 - ia) + (1 - 2 * ia),
                dsy * sy * (1 - iy) + (1 - 2 * iy)]


MODELS = {
    "radon_pooled": (radon_pooled, HERE / "out/radon_pooled/radon_pooled_data.json", 3),
    "earn_height": (earn_height, None, 3),
    "ch09_m9_4": (ch09_m9_4, REPO / "tests/rethinking/ch09_m9_4.json", 3),
    "radon_county": (radon_county, None, None),
}


def ulp(v):
    f = abs(float(v))
    return math.nextafter(f, math.inf) - f


def main():
    status = {s["name"]: s for s in json.loads((HERE / "stage1_status.json").read_text())}
    rows = []
    print(f"{'model':14s} pt {'max|g|':>11s} | error in ULP of the largest entry"
          f" (max over entries) | worst per-entry ULP")
    print(f"{'':14s}    {'':>11s} | {'stanli':>8s} {'st.fast':>8s} {'centred':>8s} "
          f"{'centr+d':>8s} {'raw':>10s} | {'stanli':>8s} {'centred':>8s} {'centr+d':>8s}")
    for name, (fn, data, npar) in MODELS.items():
        data = pathlib.Path(data or status[name]["data"])
        d = json.loads(data.read_text())
        n = npar or d["J"] + 3
        for pt in (0, 1, 2):
            u = [eval_point(i, pt) for i in range(n)]
            ref = fn(d, u, "hp")
            big = max(abs(v) for v in ref)
            unit = mpf(ulp(big))
            got = {"stanli": stanli(name, data, pt, False)[1:],
                   "st.fast": stanli(name, data, pt, True)[1:]}
            for v in ("centred", "centred+d", "raw"):
                got[v] = fn(d, u, v)
            scaled = {k: float(max(abs(mpf(a) - b) for a, b in zip(g, ref)) / unit)
                      for k, g in got.items()}
            entry = {k: float(max(abs(mpf(a) - b) / mpf(ulp(b)) for a, b in zip(g, ref)
                                  if b != 0))
                     for k, g in got.items()}
            rows.append(dict(model=name, point=pt, max_abs_grad=float(big),
                             scaled_ulp=scaled, entry_ulp=entry,
                             groups=len(d["_st"]), n_obs=sum(g["n"] for g in d["_st"])))
            print(f"{name:14s} {pt:2d} {float(big):11.4g} | {scaled['stanli']:8.2f} "
                  f"{scaled['st.fast']:8.2f} {scaled['centred']:8.2f} "
                  f"{scaled['centred+d']:8.2f} {scaled['raw']:10.3g} | "
                  f"{entry['stanli']:8.2f} {entry['centred']:8.2f} "
                  f"{entry['centred+d']:8.2f}")
    (HERE / "numerics.json").write_text(json.dumps(rows, indent=1))


def stress():
    """Near-mode, mean >> sd: where the raw-moment form cancels.

    Not a corpus point (stanli_check cannot be pointed at it in this build):
    the per-observation column is a float64 left-to-right loop standing in
    for the current path.  Model: y ~ normal(mu, sigma), data ch09_m9_4's y
    shifted by `shift`, evaluated at mu = ybar + 0.1 sd, sigma = sd.
    """
    y0 = json.loads((REPO / "tests/rethinking/ch09_m9_4.json").read_text())["y"]
    print("\nsynthetic near-mode stress (gradient w.r.t. mu and sigma; ULP of "
          "the largest entry)")
    print(f"{'shift':>8s} {'max|g|':>10s} {'per-obs':>10s} {'centred':>10s} "
          f"{'centr+d':>10s} {'raw':>12s}")
    out = []
    for shift in (0.0, 1e3, 1e6, 1e9):
        y = [v + shift for v in y0]
        n = len(y)
        ybar = math.fsum(y) / n
        sd = math.sqrt(math.fsum((v - ybar) ** 2 for v in y) / n)
        mu, sigma = ybar + 0.1 * sd, sd
        dmu, dsig = lik_hp(y, lambda i: mpf(mu), mpf(sigma))
        ref = [sum(dmu), dsig]
        unit = mpf(ulp(max(abs(v) for v in ref)))
        a = b = 0.0
        for v in y:
            r = v - mu
            a += r / (sigma * sigma)
            b += -1.0 / sigma + r * r / (sigma * sigma * sigma)
        st = stats(y, [0] * n)
        got = {"per-obs": [a, b]}
        for var in ("centred", "centred+d"):
            t, s = lik_centred(st, [mu], sigma, var == "centred+d")
            got[var] = [t[0], s]
        t, s = lik_raw(st, [mu], sigma)
        got["raw"] = [t[0], s]
        e = {k: float(max(abs(mpf(p) - q) for p, q in zip(g, ref)) / unit)
             for k, g in got.items()}
        out.append(dict(shift=shift, err=e))
        print(f"{shift:8.0e} {float(max(abs(v) for v in ref)):10.4g} "
              f"{e['per-obs']:10.3g} {e['centred']:10.3g} {e['centred+d']:10.3g} "
              f"{e['raw']:12.3g}")
    (HERE / "numerics_stress.json").write_text(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
    stress()
