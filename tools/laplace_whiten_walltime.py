#!/usr/bin/env python3
"""Wall-clock comparison of the default sampler, a dense metric and the
sparse whitened model.

For each model, with q_hat and the precision Q from laplace_whiten:

  a  default: diag_e, warmup 1000, 4 chains x 1000, parallel_chains=4
  b  dense:   inner model, dense_e with inv_metric = inverse of the repaired
              Q, step-size-only adaptation, warmup 200
  c  sparse:  whitened model, unit_e, step-size-only adaptation, warmup 200

Every timing is repeated and the median reported. Run it with nothing else
heavy on the machine; the load average is printed with the results.

    python tools/laplace_whiten_walltime.py --findings DIR [--configs a,b,c]
        [--builder-args '{"hessian": "coloured"}'] [--models gauss_slopes_c]
"""
import argparse
import json
import os
import pathlib
import statistics
import subprocess
import sys
import time
import warnings

import numpy as np

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "python"))
import stanli  # noqa: E402

MODELS = {
    "gauss_slopes_c": ("gauss_slopes_c.stan", "gauss_slopes.json", ["r_1"]),
    "bern_glmm_big": ("bern_glmm_big.stan", "bern_glmm_big.json", ["z_1"]),
    "gauss_slopes_big_c": ("gauss_slopes_big_c.stan", "gauss_slopes_big.json",
                           ["r_1"]),
}


def load(name, findings):
    stan, data, latent = MODELS[name]
    base = pathlib.Path(findings) / "precond2" / "brms"
    m = stanli.Model(stan_code=(base / stan).read_text(), data=base / data)
    return m, latent


def run(model, warmup, seed, **kw):
    t = time.time()
    fit = model.sample(chains=4, warmup=warmup, samples=1000, seed=seed,
                       save_warmup=True, parallel_chains=4, refresh=0, **kw)
    wall = time.time() - t
    st = fit.sampler_stats
    nl = stanli.SAMPLER_COLUMNS.index("n_leapfrog__")
    post = stanli.Fit(fit.names,
                      np.ascontiguousarray(fit.draws()[:, warmup:, :]),
                      st[:, warmup:], fit.max_depth, fit.seed)
    ess = np.asarray(post.summary().ess_bulk)
    keep = [i for i, nm in enumerate(fit.names) if nm in set(
        model.constrained_names)]
    ess = ess[keep]
    return dict(wall=wall, grads=float(st[:, :, nl].sum()),
                ess=float(np.nanmin(ess)))


def median_run(model, warmup, seed, repeats, **kw):
    runs = [run(model, warmup, seed, **kw) for _ in range(repeats)]
    out = dict(runs[0])
    out["walls"] = [r["wall"] for r in runs]
    out["wall"] = statistics.median(out["walls"])
    return out


def grad_micro(model, q, calls=300):
    for _ in range(20):
        model.log_prob_grad(q)
    ts = []
    for _ in range(calls):
        t = time.perf_counter()
        model.log_prob_grad(q)
        ts.append(time.perf_counter() - t)
    return statistics.median(ts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--findings", required=True)
    ap.add_argument("--models", nargs="*", default=list(MODELS))
    ap.add_argument("--configs", default="a,b,c")
    ap.add_argument("--builder-args", default="{}")
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--json", type=pathlib.Path)
    a = ap.parse_args()
    bargs = json.loads(a.builder_args)
    configs = a.configs.split(",")
    uptime = subprocess.run(["uptime"], capture_output=True,
                            text=True).stdout.strip()
    print(uptime, flush=True)
    results = {"uptime": uptime, "builder_args": bargs, "models": {}}
    for name in a.models:
        m, latent = load(name, a.findings)
        n = m.n_unconstrained
        res = dict(n=n)
        walls, rep, w = [], None, None
        for _ in range(a.repeats):
            t = time.time()
            with warnings.catch_warnings(record=True) as caught:
                warnings.simplefilter("always")
                w, rep = stanli.laplace_whiten(
                    m, latent, seed=a.seed, keep_precision=("b" in configs),
                    **bargs)
            walls.append(time.time() - t)
        res.update(builder_wall=statistics.median(walls), builder_walls=walls,
                   builder_grads=rep.gradients, nnz_frac=rep.nnz_L / n ** 2,
                   repair_triggered=bool(rep.n_clipped),
                   n_repaired=rep.n_clipped,
                   warnings=[str(c.message)[:100] for c in caught][:1])
        if "a" in configs:
            res["a"] = median_run(m, 1000, a.seed, a.repeats, metric="diag_e")
        if "b" in configs:
            Qinv = np.linalg.inv(rep.precision)
            Qinv = (Qinv + Qinv.T) / 2
            res["b"] = median_run(m, 200, a.seed, a.repeats, metric="dense_e",
                                  inv_metric=Qinv, adapt_metric=False)
        if "c" in configs:
            res["c"] = median_run(w, 200, a.seed, a.repeats, metric="unit_e",
                                  adapt_metric=False)
        res["grad_inner_us"] = 1e6 * grad_micro(m, rep.q_hat)
        res["grad_whitened_us"] = 1e6 * grad_micro(w, np.zeros(n))
        results["models"][name] = res
        print(name, json.dumps(res, default=float), flush=True)
    print("\n| model | n | config | builder s | builder grads | sampling s "
          "| gradients | min ESS | ESS/s incl. builder | nnz(L)/n^2 | "
          "repaired |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for name, r in results["models"].items():
        for c, label in (("a", "default"), ("b", "dense"), ("c", "sparse")):
            if c not in r:
                continue
            b = 0.0 if c == "a" else r["builder_wall"]
            bg = 0 if c == "a" else r["builder_grads"]["total"]
            tot = r[c]["wall"] + b
            fill = "" if c == "a" else f"{r['nnz_frac']:.3f}"
            fixed = "" if c == "a" else r["n_repaired"]
            print(f"| {name} | {r['n']} | {label} | {b:.1f} | {bg} | "
                  f"{r[c]['wall']:.1f} | {r[c]['grads']:.0f} | "
                  f"{r[c]['ess']:.0f} | {r[c]['ess'] / tot:.2f} | {fill} | "
                  f"{fixed} |")
    print("\n| model | inner grad us | whitened grad us |")
    print("|---|---|---|")
    for name, r in results["models"].items():
        print(f"| {name} | {r['grad_inner_us']:.0f} | "
              f"{r['grad_whitened_us']:.0f} |")
    if a.json:
        a.json.write_text(json.dumps(results, indent=1, default=float))


if __name__ == "__main__":
    main()
