#!/usr/bin/env python3
"""Tests for stanli.laplace_whiten, the experimental whitening builder.

Needs numpy and scipy. Run against an installed wheel or a local build:

    PYTHONPATH=python python3 tests/test_laplace_whiten.py
"""
import pathlib
import sys
import warnings

try:
    import numpy as np
    import scipy.optimize
    import scipy.sparse
except ImportError as exc:
    print(f"SKIP {pathlib.Path(__file__).name}: needs numpy and scipy ({exc})")
    sys.exit(0)

import stanli

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_whiten import radon_data  # noqa: E402

CENTRED = """
data {
  int<lower=0> J;
  int<lower=0> N;
  array[N] int<lower=1, upper=J> county_idx;
  vector[N] log_uppm;
  vector[N] floor_measure;
  vector[N] log_radon;
}
parameters {
  real mu_alpha;
  real<lower=0> sigma_alpha;
  vector[J] alpha;
  vector[2] beta;
  real<lower=0> sigma_y;
}
model {
  sigma_alpha ~ normal(0, 1);
  sigma_y ~ normal(0, 1);
  mu_alpha ~ normal(0, 10);
  beta ~ normal(0, 10);
  alpha ~ normal(mu_alpha, sigma_alpha);
  log_radon ~ normal(alpha[county_idx] + log_uppm * beta[1]
                     + floor_measure * beta[2], sigma_y);
}
"""

NONCENTRED = """
data {
  int<lower=0> J;
  int<lower=0> N;
  array[N] int<lower=1, upper=J> county_idx;
  vector[N] log_uppm;
  vector[N] floor_measure;
  vector[N] log_radon;
}
parameters {
  vector[J] alpha_raw;
  vector[2] beta;
  real mu_alpha;
  real<lower=0> sigma_alpha;
  real<lower=0> sigma_y;
}
transformed parameters {
  vector[J] alpha = mu_alpha + sigma_alpha * alpha_raw;
}
model {
  sigma_alpha ~ normal(0, 1);
  sigma_y ~ normal(0, 1);
  mu_alpha ~ normal(0, 10);
  beta ~ normal(0, 10);
  alpha_raw ~ normal(0, 1);
  log_radon ~ normal(alpha[county_idx] + log_uppm * beta[1]
                     + floor_measure * beta[2], sigma_y);
}
"""

STRUCTURED = """
parameters {
  real<lower=0> tau;
  simplex[4] s;
  cholesky_factor_corr[3] Lc;
  vector[2] v;
}
model {
  tau ~ exponential(1);
  Lc ~ lkj_corr_cholesky(2);
  v ~ normal(0, 1);
}
"""

SADDLE = """
parameters {
  real x;
  real y;
}
model {
  target += x^2 / 2 - x^4 / 4;
  y ~ normal(0.5 * x, 1);
}
"""

J = 12


def centred():
    return stanli.Model(stan_code=CENTRED, data=radon_data(J=J, N=120, seed=5))


def test_unconstrained_layout():
    m = stanli.Model(stan_code=STRUCTURED)
    layout = m._unconstrained_layout()
    assert layout == [("tau", 1), ("s", 3), ("Lc", 3), ("v", 2)], layout
    assert sum(k for _, k in layout) == m.n_unconstrained


def test_latent_selects_index_range():
    m = stanli.Model(stan_code=STRUCTURED)
    _, report = stanli.laplace_whiten(m, latent=["s", "v"],
                                      init=np.zeros(m.n_unconstrained))
    assert list(report.latent_index) == [1, 2, 3, 7, 8], report.latent_index
    assert report.n_latent == 5 and report.n_theta == 4


def test_theta_hat_matches_dense_reference():
    m = centred()
    n = m.n_unconstrained
    whitened, report = stanli.laplace_whiten(m, latent=["alpha"], seed=1)
    iu = np.arange(2, 2 + J)
    it = np.setdiff1d(np.arange(n), iu)
    assert list(report.latent_index) == list(iu)

    def best_u(theta, u0):
        q = np.empty(n)
        q[it] = theta

        def f(u):
            q[iu] = u
            lp, g = m.log_prob_grad(q)
            return -lp, -g[iu]
        r = scipy.optimize.minimize(f, u0, jac=True, method="L-BFGS-B",
                                    options=dict(maxiter=2000, gtol=1e-10,
                                                 ftol=1e-15, maxcor=20))
        q[iu] = r.x
        return q.copy()

    def neg_marginal(theta):
        q = best_u(theta, np.zeros(J))
        lp, _, h = m.log_prob_hessian(q)
        _, logdet = np.linalg.slogdet(-h[np.ix_(iu, iu)])
        return -(lp - 0.5 * logdet)

    start = np.asarray(report.theta_start)
    ref = scipy.optimize.minimize(neg_marginal, start, method="L-BFGS-B",
                                  options=dict(maxiter=200, ftol=1e-14,
                                               gtol=1e-8, eps=1e-6))
    gap = np.abs(np.asarray(report.theta_hat) - ref.x).max()
    value_gap = neg_marginal(np.asarray(report.theta_hat)) - ref.fun
    print(f"  theta_hat max gap {gap:.2e}, objective excess {value_gap:.2e}")
    assert gap <= 2e-3, (report.theta_hat, ref.x)
    assert value_gap <= 1e-5


def test_precision_is_spd_and_factor_reproduces_it():
    m = centred()
    n = m.n_unconstrained
    whitened, report = stanli.laplace_whiten(m, latent=["alpha"], seed=1,
                                             keep_precision=True)
    assert report.n_clipped == 0
    Q = report.precision
    assert np.linalg.eigvalsh(Q).min() > 0
    L = report.L.toarray()
    assert np.allclose(np.triu(L, 1), 0)
    P = Q[np.ix_(report.perm, report.perm)]
    assert np.abs(L @ L.T - P).max() <= 1e-9 * np.abs(Q).max()
    assert sorted(report.perm) == list(range(n))
    assert 0 < report.nnz_L / n ** 2 <= 1
    z = np.zeros(n)
    _, _, h = whitened.log_prob_hessian(z)
    assert np.abs(-h - np.eye(n)).max() <= 1e-3, np.abs(-h - np.eye(n)).max()


def test_report_counts_gradients():
    m = centred()
    n = m.n_unconstrained
    _, report = stanli.laplace_whiten(m, latent=["alpha"], seed=1,
                                      hessian="dense", pattern="dense")
    g = report.gradients
    assert g["pattern_hessian"] == 1 + 2 * n
    assert g["final_hessian"] == 1 + 2 * n
    assert g["marginal_mode"] > 0
    assert g["total"] == (g["pattern_hessian"] + g["marginal_mode"]
                          + g["final_hessian"])
    assert report.pathfinder_seconds > 0 and report.wall_seconds > 0
    assert len(report.theta_hat) == report.n_theta


def test_coloured_hessian_uses_fewer_gradients_than_dense():
    m = centred()
    n = m.n_unconstrained
    _, report = stanli.laplace_whiten(m, latent=["alpha"], seed=1)
    g = report.gradients
    assert g["final_hessian"] < 1 + 2 * n


def test_clipping_path_warns_and_counts():
    m = stanli.Model(stan_code=SADDLE)
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        whitened, report = stanli.laplace_whiten(
            m, latent=["y"], init=np.zeros(2), repair="clip",
            keep_precision=True)
    assert report.n_clipped >= 1
    assert any("clipped" in str(w.message) for w in caught), \
        [str(w.message) for w in caught]
    assert np.linalg.eigvalsh(report.precision).min() > 0
    assert whitened.n_unconstrained == 2
    lp, grad = whitened.log_prob_grad(np.array([0.1, -0.2]))
    assert np.isfinite(lp) and np.all(np.isfinite(grad))


def test_abs_repair_keeps_the_scale_of_a_negative_direction():
    m = stanli.Model(stan_code=SADDLE)
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        _, clipped = stanli.laplace_whiten(
            m, latent=["y"], init=np.zeros(2), repair="clip",
            keep_precision=True)
        _, absolute = stanli.laplace_whiten(
            m, latent=["y"], init=np.zeros(2), repair="abs",
            keep_precision=True)
    assert absolute.n_clipped >= 1
    assert any("absolute" in str(w.message) for w in caught)
    w_abs = np.linalg.eigvalsh(absolute.precision)
    w_clip = np.linalg.eigvalsh(clipped.precision)
    assert w_abs.min() > 0
    assert abs(w_abs.min() - abs(absolute.min_eigenvalue)) \
        <= 1e-9 * w_abs.max()
    assert w_clip.min() >= 1e-3 * w_clip.max() * (1 - 1e-9)


def test_unknown_repair_is_refused():
    m = stanli.Model(stan_code=SADDLE)
    refused("repair", m, latent=["y"], init=np.zeros(2), repair="soft")


def test_no_warning_when_precision_is_positive_definite():
    m = centred()
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        stanli.laplace_whiten(m, latent=["alpha"], seed=1)
    assert not [w for w in caught if "clipped" in str(w.message)]


def refused(match, *args, **kwargs):
    try:
        stanli.laplace_whiten(*args, **kwargs)
    except (ValueError, TypeError) as e:
        assert match in str(e), str(e)
    else:
        raise AssertionError("expected laplace_whiten to refuse: " + match)


def test_bad_latent_names_are_refused():
    m = stanli.Model(stan_code=NONCENTRED, data=radon_data(J=J, N=120, seed=5))
    refused("nope", m, latent=["nope"])
    refused("transformed parameter", m, latent=["alpha"])
    refused("data", m, latent=["J"])
    refused("at least one", m, latent=[])
    refused("twice", m, latent=["alpha_raw", "alpha_raw"])
    refused("list of parameter names", m, latent="alpha_raw")
    refused("already whitened", m.whiten(np.zeros(m.n_unconstrained),
                                         np.arange(m.n_unconstrained),
                                         scipy.sparse.identity(
                                             m.n_unconstrained, format="csc")),
            latent=["alpha_raw"])


def test_noncentred_latent_block_works():
    m = stanli.Model(stan_code=NONCENTRED, data=radon_data(J=J, N=120, seed=5))
    whitened, report = stanli.laplace_whiten(m, latent=["alpha_raw"], seed=1)
    assert list(report.latent_index) == list(range(J))
    fit = whitened.sample(chains=2, warmup=200, samples=200, seed=1,
                          metric="unit_e", adapt_metric=False, refresh=0)
    assert fit.names == m.sample(chains=1, warmup=10, samples=10,
                                 refresh=0).names


def test_all_latent_has_no_hyperparameters():
    m = stanli.Model(stan_code=STRUCTURED)
    whitened, report = stanli.laplace_whiten(
        m, latent=["tau", "s", "Lc", "v"], init=np.zeros(m.n_unconstrained))
    assert report.n_theta == 0 and len(report.theta_hat) == 0
    assert whitened.n_unconstrained == m.n_unconstrained


def main():
    failed = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_"):
            continue
        try:
            fn()
            print(f"ok   {name}")
        except Exception as e:  # noqa: BLE001 - report and count everything
            failed += 1
            print(f"FAIL {name}: {type(e).__name__}: {e}")
    print(f"\n{'FAILED' if failed else 'all passed'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
