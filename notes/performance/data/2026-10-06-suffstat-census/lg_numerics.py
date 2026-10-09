#!/usr/bin/env python3
"""High-precision check of the linear-Gaussian rewrites (throwaway spike).

Likelihood  y ~ normal(c + Z theta, sigma), Z an N x P data matrix taken
from the lowered graph (analyze2).  Against a 70-digit mpmath reference of
the whole model gradient (unconstrained space), in ULP of the largest
gradient entry:

  stanli     build-spike/stanli_check --mir, the current path
  gram       raw Gram: y'y, Z'y, Z'Z prepared exactly and rounded once;
             Q = y'y - 2 theta'Z'y + theta'Z'Z theta, grad = (Z'y - Z'Z theta)/s^2
  qr         triangular form: R = qr([Z y]) (numpy Householder, double);
             v = R [-theta; 1], Q = v'v, grad = R[:, :P]' v / s^2
  qr-exact   the same with R = chol([Z y]'[Z y]) computed at 60 digits and
             rounded once (what a double-double preparation would give)
  lsc        least-squares-centred triangular form: theta_hat (double),
             d = Z'(y - Z theta_hat), RSS, Rz = qr(Z) (numpy, double);
             w = Rz (theta - theta_hat), Q = RSS - 2 (theta - theta_hat)'d + w'w,
             grad = (d - Rz'w) / s^2
  lsc-exact  the same with Rz = chol(Z'Z) at 60 digits, rounded once

Corpus points 0-2 use stanli_check.  The near-mode point (theta = least
squares + 1e-3 perturbation, sigma = residual sd) is where the sampler
lives and where the raw Gram form cancels; stanli_check cannot be pointed
at it in this build, so a float64 per-observation loop stands in there.
"""
import json
import math
import pathlib
import subprocess

import numpy as np
from mpmath import mp, mpf, matrix, cholesky, fdot, exp

import analyze2 as A2
from analyze import U, mix

mp.dps = 70
HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
CHECK = REPO / "build-spike/stanli_check"
status = {s["name"]: s for s in json.loads((HERE / "stage1_status.json").read_text())}


def eval_point(i, variant):
    if variant == 1:
        return 0.02 * ((i % 5) - 2)
    if variant == 2:
        return 0.0
    return 0.1 + 0.05 * (i % 7) - 0.15 * (i % 3)


def stanli(model, pt):
    cmd = [str(CHECK), str(HERE / "out" / model / "model.stan"), status[model]["data"],
           "--mir", str(HERE / "out" / model / "model.sexp"), "--point", str(pt)]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    return [float(x) for x in [ln for ln in out.splitlines()
                               if ln.startswith("OK")][-1].split()[2:]]


def t_prior(x, nu, m, s):   # d/dx student_t_lpdf(x | nu, m, s)
    return -(nu + 1) * (x - m) / (nu * s * s + (x - m) ** 2)


# prior gradient contributions: f(theta list, sigma, data) -> (list, dsigma)
def pri_kilpisjarvi(th, s, d):
    return [-(th[0] - d["pmualpha"]) / d["psalpha"] ** 2,
            -(th[1] - d["pmubeta"]) / d["psbeta"] ** 2], 0


def pri_kid(th, s, d):
    return [0] * len(th), -2 * s / (2.5 ** 2 + s * s)


def pri_none(th, s, d):
    return [0] * len(th), 0


def pri_diamonds(th, s, d):
    return [-t for t in th[:-1]] + [t_prior(th[-1], 3, 8, 10)], t_prior(s, 3, 0, 10)


MODELS = {"kilpisjarvi": pri_kilpisjarvi, "kidscore_interaction": pri_kid,
          "mesquite": pri_none, "diamonds": pri_diamonds}


def design(model):
    A = A2.Analysis2(HERE / "out" / model / "census.txt.gz")
    A.prepare()
    g = max((g for g in A.lg_groups if not g["lognormal"]), key=lambda g: g["N"])
    pos, off = {}, 0
    for s, (ln, is_param, _a) in enumerate(A.slots):
        if is_param:
            for k, key in enumerate(mix(mix(U(11), U(s)), np.arange(ln, dtype=U))):
                pos[int(key)] = off + k
            off += ln
    cols = [pos[int(k)] for k in g["_leaves"]]      # leaf column -> parameter index
    assert sorted(cols) == list(range(g["P"])) and off == g["P"] + 1, (cols, off)
    Z = np.zeros((g["N"], g["P"]))
    r, c, v = g["_Z"]
    Z[r, np.array(cols)[c]] = v
    return Z, g["_y"] - 0.0, g["_const"]


def prepare(Z, y, c):
    N, P = Z.shape
    Zm = [[mpf(float(v)) for v in row] for row in Z]
    yt = [mpf(float(a)) - mpf(float(b)) for a, b in zip(y, c)]
    aug = [[Zm[i][j] for i in range(N)] for j in range(P)] + [yt]
    Gx = matrix(P + 1, P + 1)
    for a in range(P + 1):
        for b in range(a, P + 1):
            Gx[a, b] = Gx[b, a] = fdot(aug[a], aug[b])
    old = mp.dps
    L = cholesky(Gx)
    mp.dps = old
    Rx = np.array([[float(L[j, i]) for j in range(P + 1)] for i in range(P + 1)])
    Gf = np.array([[float(Gx[i, j]) for j in range(P + 1)] for i in range(P + 1)])
    ytf = np.array([float(v) for v in yt])
    Rd = np.linalg.qr(np.column_stack([Z, ytf]), mode="r")
    # least-squares-centred triangular form: theta_hat rounded to double,
    # d = Z'(y - Z theta_hat) and RSS = |y - Z theta_hat|^2 exact then rounded,
    # Rz = chol(Z'Z) (60 digits, rounded) or numpy QR of Z (double)
    th_hat = np.linalg.lstsq(Z, ytf, rcond=None)[0]
    thm = [mpf(float(t)) for t in th_hat]
    res = [yi - fdot(row, thm) for row, yi in zip(Zm, yt)]
    dvec = np.array([float(fdot(aug[j], res)) for j in range(P)])
    rss = float(fdot(res, res))
    Lz = cholesky(Gx[:P, :P])
    Rzx = np.array([[float(Lz[j, i]) for j in range(P)] for i in range(P)])
    Rzd = np.linalg.qr(Z, mode="r")
    return dict(Zm=Zm, yt=yt, G=Gf, Rx=Rx, Rd=Rd, ytf=ytf, th_hat=th_hat, d=dvec,
                rss=rss, Rzx=Rzx, Rzd=Rzd)


def grad_hp(prep, th, us, N):
    th = [mpf(t) for t in th]
    s = exp(mpf(us))
    r = [yi - fdot(row, th) for row, yi in zip(prep["Zm"], prep["yt"])]
    P = len(th)
    g = [fdot([row[j] for row in prep["Zm"]], r) / s ** 2 for j in range(P)]
    return g, -N / s + fdot(r, r) / s ** 3, s


def variants(prep, Z, th, us, N):
    th = np.array(th)
    s = math.exp(us)
    P = th.size
    G = prep["G"]
    out = {}
    q = G[P, P] - 2.0 * th @ G[:P, P] + th @ G[:P, :P] @ th
    out["gram"] = ((G[:P, P] - G[:P, :P] @ th) / (s * s), -N / s + q / s ** 3)
    for key, R in (("qr", prep["Rd"]), ("qr-exact", prep["Rx"])):
        v = R @ np.concatenate([-th, [1.0]])
        out[key] = (R[:, :P].T @ v / (s * s), -N / s + (v @ v) / s ** 3)
    for key, R in (("lsc", prep["Rzd"]), ("lsc-exact", prep["Rzx"])):
        dl = th - prep["th_hat"]
        w = R @ dl
        out[key] = ((prep["d"] - R.T @ w) / (s * s),
                    -N / s + (prep["rss"] - 2.0 * (dl @ prep["d"]) + w @ w) / s ** 3)
    acc_g, acc_q = np.zeros(P), 0.0
    for i in range(N):
        r = prep["ytf"][i] - Z[i] @ th
        acc_g += Z[i] * r
        acc_q += r * r
    out["per-obs"] = (acc_g / (s * s), -N / s + acc_q / s ** 3)
    return out, s


def ulp(v):
    f = abs(float(v))
    return math.nextafter(f, math.inf) - f


def main():
    rows = []
    print(f"{'model':22s} {'point':>9s} {'max|g|':>10s} | error, ULP of the largest "
          f"gradient entry")
    print(f"{'':22s} {'':>9s} {'':>10s} | {'stanli':>9s} {'per-obs':>9s} {'gram':>10s} "
          f"{'qr':>9s} {'qr-exact':>9s} {'lsc':>9s} {'lsc-exact':>9s}")
    for model, prior in MODELS.items():
        d = json.loads(pathlib.Path(status[model]["data"]).read_text())
        Z, y, c = design(model)
        N, P = Z.shape
        sv = np.linalg.svd(Z, compute_uv=False)
        prep = prepare(Z, y, c)
        ls = np.linalg.lstsq(Z, prep["ytf"], rcond=None)[0]
        sd = float(np.sqrt(((prep["ytf"] - Z @ ls) ** 2).mean()))
        points = [(f"pt{k}", [eval_point(i, k) for i in range(P)], eval_point(P, k), k)
                  for k in (0, 1, 2)]
        points.append(("near-mode", list(ls * (1 + 1e-3)), math.log(sd), None))
        print(f"# {model}: N={N} P={P} cond(Z)={sv[0] / sv[-1]:.3g} "
              f"|y|^2/RSS={float(prep['ytf'] @ prep['ytf']) / (N * sd * sd):.3g}")
        for label, th, us, k in points:
            gh, dsh, sh = grad_hp(prep, th, us, N)
            ph, pdh = prior([mpf(t) for t in th], sh, {a: (mpf(b) if isinstance(b, float) else b)
                                                         for a, b in d.items()
                                                         if not isinstance(b, list)})
            ref = [a + b for a, b in zip(gh, ph)] + [(dsh + pdh) * sh + 1]
            unit = mpf(ulp(max(abs(v) for v in ref)))
            var, s = variants(prep, Z, th, us, N)
            pf, pdf = prior(list(th), s, {a: b for a, b in d.items()
                                          if not isinstance(b, list)})
            err = {}
            for key, (g, ds) in var.items():
                full = [float(a) + float(b) for a, b in zip(g, pf)] + [(ds + pdf) * s + 1.0]
                err[key] = float(max(abs(mpf(a) - b) for a, b in zip(full, ref)) / unit)
            err["stanli"] = (float(max(abs(mpf(a) - b) for a, b in zip(stanli(model, k), ref))
                                   / unit) if k is not None else float("nan"))
            rows.append(dict(model=model, point=label, N=N, P=P,
                             cond=float(sv[0] / sv[-1]),
                             max_abs_grad=float(max(abs(v) for v in ref)), err=err))
            print(f"{model:22s} {label:>9s} {float(max(abs(v) for v in ref)):10.4g} | "
                  f"{err['stanli']:9.2f} {err['per-obs']:9.2f} {err['gram']:10.3g} "
                  f"{err['qr']:9.2f} {err['qr-exact']:9.2f} {err['lsc']:9.2f} "
                  f"{err['lsc-exact']:9.2f}", flush=True)
    (HERE / "lg_numerics.json").write_text(json.dumps(rows, indent=1))


if __name__ == "__main__":
    main()
