"""Generated models for tools/laplace_whiten_acceptance.py.

The four non-posteriordb benchmark models of the #540 follow-up. Their Stan
code is checked in under tools/laplace_whiten_stan/ (brms 2.23 output for the
first three, hand-written for the spatial model). The data are simulated here
with numpy and fixed seeds, to the same designs as the benchmark's, which
drew them with R (brms make_standata). The values therefore differ from the
benchmark's data, the posteriors have the same structure, and the numbers
agree only approximately.

    crossed_c            100 subjects x 50 items, one observation per pair,
                         centred crossed intercepts, gaussian
    gauss_slopes         300 subjects x 20 observations, correlated
                         intercept and slope (non-centred, LKJ), gaussian
    bern_glmm            500 groups x 4 observations, non-centred
                         intercepts, bernoulli
    spatial_binom_gmrf   20 x 20 grid, binomial counts, centred GMRF field
"""
import json
import pathlib

import numpy as np

STAN_DIR = pathlib.Path(__file__).resolve().parent / "laplace_whiten_stan"

NAMES = ("crossed_c", "gauss_slopes", "bern_glmm", "spatial_binom_gmrf")


def _design(x):
    return [[1.0, float(v)] for v in x]


def _crossed(rng):
    n_subj, n_item = 100, 50
    subj = np.tile(np.arange(n_subj), n_item)
    item = np.repeat(np.arange(n_item), n_subj)
    n = n_subj * n_item
    x = rng.normal(size=n)
    us = rng.normal(0, 0.7, n_subj)
    ui = rng.normal(0, 0.5, n_item)
    y = 0.5 + 0.3 * x + us[subj] + ui[item] + rng.normal(0, 1, n)
    return dict(
        N=n, Y=y.tolist(), K=2, X=_design(x), Kc=1, N_1=n_item, M_1=1,
        J_1=(item + 1).tolist(), Z_1_1=np.ones(n).tolist(), N_2=n_subj,
        M_2=1, J_2=(subj + 1).tolist(), Z_2_1=np.ones(n).tolist(),
        prior_only=0)


def _gauss_slopes(rng):
    n_subj, per = 300, 20
    subj = np.repeat(np.arange(n_subj), per)
    n = n_subj * per
    x = rng.normal(size=n)
    sd = np.array([0.8, 0.4])
    corr = np.array([[1.0, 0.3], [0.3, 1.0]])
    u = rng.multivariate_normal([0, 0], np.outer(sd, sd) * corr, n_subj)
    y = 1 + 0.5 * x + u[subj, 0] + u[subj, 1] * x + rng.normal(0, 1, n)
    return dict(
        N=n, Y=y.tolist(), K=2, X=_design(x), Kc=1, N_1=n_subj, M_1=2,
        J_1=(subj + 1).tolist(), Z_1_1=np.ones(n).tolist(), Z_1_2=x.tolist(),
        NC_1=1, prior_only=0)


def _bern_glmm(rng):
    n_grp, per = 500, 4
    grp = np.repeat(np.arange(n_grp), per)
    n = n_grp * per
    x = rng.normal(size=n)
    u = rng.normal(0, 1, n_grp)
    p = 1 / (1 + np.exp(-(-0.3 + 0.8 * x + u[grp])))
    y = rng.binomial(1, p)
    return dict(
        N=n, Y=y.tolist(), K=2, X=_design(x), Kc=1, N_1=n_grp, M_1=1,
        J_1=(grp + 1).tolist(), Z_1_1=np.ones(n).tolist(), prior_only=0)


def _grid_laplacian(nr, nc):
    idx = np.arange(nr * nc).reshape(nr, nc)
    edges = [(idx[i, j], idx[i, j + 1]) for i in range(nr)
             for j in range(nc - 1)]
    edges += [(idx[i, j], idx[i + 1, j]) for i in range(nr - 1)
              for j in range(nc)]
    edges = np.array(edges)
    n = nr * nc
    lap = np.zeros((n, n))
    for a, b in edges:
        lap[a, a] += 1
        lap[b, b] += 1
        lap[a, b] -= 1
        lap[b, a] -= 1
    return edges, lap


def _spatial_binom_gmrf(rng):
    nr = nc = 20
    n = nr * nc
    edges, lap = _grid_laplacian(nr, nc)
    lam = np.clip(np.linalg.eigvalsh(lap), 0, None)
    tau, kappa = 2.0, 0.3
    chol = np.linalg.cholesky(tau * (kappa ** 2 * np.eye(n) + lap))
    phi = np.linalg.solve(chol.T, rng.normal(size=n))
    x = rng.normal(size=n)
    trials = rng.integers(5, 21, n)
    p = 1 / (1 + np.exp(-(-0.3 + 0.5 * x + phi)))
    y = rng.binomial(trials, p)
    return {"N": n, "N_edges": len(edges),
            "node1": (edges[:, 0] + 1).tolist(),
            "node2": (edges[:, 1] + 1).tolist(), "lambda": lam.tolist(),
            "x": x.tolist(), "trials": trials.astype(int).tolist(),
            "y": y.astype(int).tolist()}


_GENERATORS = {
    "crossed_c": (_crossed, 102),
    "gauss_slopes": (_gauss_slopes, 101),
    "bern_glmm": (_bern_glmm, 103),
    "spatial_binom_gmrf": (_spatial_binom_gmrf, 303),
}


def generate(name, directory):
    """Write `name`'s data to `directory` (the same bytes every time) and
    return (stan_code, path_of_data_json)."""
    make, seed = _GENERATORS[name]
    directory = pathlib.Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"{name}.data.json"
    path.write_text(json.dumps(make(np.random.default_rng(seed))))
    return (STAN_DIR / f"{name}.stan").read_text(), path
