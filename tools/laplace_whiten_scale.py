#!/usr/bin/env python3
"""Scale test for the sparse laplace_whiten builder.

Simulates the correlated random-slope model of gauss_slopes (the same Stan
program and truth, more groups), builds the whitened model with the sparse
builder and compares one gradient and a short run against the default
sampler.

    python tools/laplace_whiten_scale.py --findings DIR [--groups 6000]
"""
import argparse
import json
import pathlib
import statistics
import sys
import time
import warnings

import numpy as np

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "python"))
import stanli  # noqa: E402


def simulate(groups, per_group, seed):
    rng = np.random.RandomState(seed)
    n_obs = groups * per_group
    subj = np.repeat(np.arange(1, groups + 1), per_group)
    x = rng.randn(n_obs)
    sd = np.array([0.8, 0.4])
    cov = np.diag(sd) @ np.array([[1, 0.3], [0.3, 1]]) @ np.diag(sd)
    u = rng.multivariate_normal([0, 0], cov, groups)
    y = 1 + 0.5 * x + u[subj - 1, 0] + u[subj - 1, 1] * x + rng.randn(n_obs)
    return {"N": n_obs, "Y": y.tolist(), "K": 2, "Kc": 1,
            "X": np.column_stack([np.ones(n_obs), x]).tolist(),
            "Z_1_1": [1] * n_obs, "Z_1_2": x.tolist(), "J_1": subj.tolist(),
            "N_1": groups, "M_1": 2, "NC_1": 1, "prior_only": 0}


def micro(model, q, calls=200):
    for _ in range(10):
        model.log_prob_grad(q)
    ts = []
    for _ in range(calls):
        t = time.perf_counter()
        model.log_prob_grad(q)
        ts.append(time.perf_counter() - t)
    return statistics.median(ts)


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
    keep = [i for i, nm in enumerate(fit.names)
            if nm in set(model.constrained_names)]
    return dict(wall=wall, grads=float(st[:, :, nl].sum()),
                min_ess=float(np.nanmin(ess[keep])))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--findings", required=True)
    ap.add_argument("--groups", type=int, default=6000)
    ap.add_argument("--per-group", type=int, default=20)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--skip-default", action="store_true")
    ap.add_argument("--json", type=pathlib.Path)
    a = ap.parse_args()
    stan = (pathlib.Path(a.findings) / "precond2" / "brms"
            / "gauss_slopes_big_c.stan").read_text()
    data = simulate(a.groups, a.per_group, a.seed)
    model = stanli.Model(stan_code=stan, data=data)
    n = model.n_unconstrained
    print(f"groups {a.groups}, n = {n}", flush=True)
    t = time.time()
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        whitened, rep = stanli.laplace_whiten(model, ["r_1"], seed=a.seed)
    builder = time.time() - t
    res = dict(n=n, groups=a.groups, builder_wall=builder,
               pathfinder_seconds=rep.pathfinder_seconds,
               gradients=rep.gradients, n_colors=rep.n_colors,
               nnz_L=rep.nnz_L, nnz_frac=rep.nnz_L / n ** 2, rank=rep.rank,
               repaired=rep.n_clipped,
               warnings=[str(c.message)[:120] for c in caught][:2])
    print(json.dumps(res, default=float), flush=True)
    res["grad_inner_us"] = 1e6 * micro(model, rep.q_hat)
    res["grad_whitened_us"] = 1e6 * micro(whitened, np.zeros(n))
    print(res["grad_inner_us"], res["grad_whitened_us"], flush=True)
    res["sparse"] = run(whitened, 200, a.seed, metric="unit_e",
                        adapt_metric=False)
    print("sparse", res["sparse"], flush=True)
    if not a.skip_default:
        res["default"] = run(model, 1000, a.seed, metric="diag_e")
        print("default", res["default"], flush=True)
    if a.json:
        a.json.write_text(json.dumps(res, indent=1, default=float))


if __name__ == "__main__":
    main()
