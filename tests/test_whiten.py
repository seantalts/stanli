#!/usr/bin/env python3
"""Tests for Model.whiten, the experimental Laplace-whitening transform.

Needs numpy and scipy. Run against an installed wheel or a local build:

    PYTHONPATH=python python3 tests/test_whiten.py
"""
import pathlib
import sys
import warnings

try:
    import numpy as np
    import scipy.linalg
    import scipy.sparse
except ImportError as exc:
    print(f"SKIP {pathlib.Path(__file__).name}: needs numpy and scipy ({exc})")
    sys.exit(0)

import stanli

FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures"

ES_GQ = """
data {
  int<lower=0> J;
  vector[J] y;
  vector<lower=0>[J] sigma;
}
parameters {
  real mu;
  real<lower=0> tau;
  vector[J] theta_tilde;
}
transformed parameters {
  vector[J] theta = mu + tau * theta_tilde;
}
model {
  mu ~ normal(0, 5);
  tau ~ cauchy(0, 5);
  theta_tilde ~ normal(0, 1);
  y ~ normal(theta, sigma);
}
generated quantities {
  real tau_sq = square(tau);
  vector[J] y_rep;
  for (j in 1:J) y_rep[j] = normal_rng(theta[j], sigma[j]);
}
"""

RADON = """
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


def radon_data(J=20, N=240, seed=11):
    rng = np.random.RandomState(seed)
    county = np.concatenate([np.arange(1, J + 1), rng.randint(1, J + 1, N - J)])
    alpha = 1.3 + 0.4 * rng.randn(J)
    log_uppm = 0.3 * rng.randn(N)
    floor = rng.randint(0, 2, N).astype(float)
    y = (alpha[county - 1] + 0.7 * log_uppm - 0.6 * floor
         + 0.7 * rng.randn(N))
    return {"J": J, "N": N, "county_idx": county.tolist(),
            "log_uppm": log_uppm.tolist(), "floor_measure": floor.tolist(),
            "log_radon": y.tolist()}


def es_model():
    return stanli.Model(stan_code=ES_GQ,
                        data=FIXTURES / "eight_schools.json")


def radon_model():
    return stanli.Model(stan_code=RADON, data=radon_data())


def random_factor(n, rng, density=0.2):
    lower = np.tril(rng.randn(n, n) * 0.3 * (rng.rand(n, n) < density), -1)
    lower += np.diag(rng.uniform(0.8, 1.6, n))
    return lower


class Fixture:
    def __init__(self, inner, seed):
        rng = np.random.RandomState(seed)
        n = inner.n_unconstrained
        self.inner = inner
        self.n = n
        self.q_hat = 0.2 * rng.randn(n)
        self.perm = rng.permutation(n)
        self.dense = random_factor(n, rng)
        self.sparse = scipy.sparse.csc_matrix(self.dense)
        self.model = inner.whiten(self.q_hat, self.perm, self.sparse)
        self.rng = rng

    def q_of_z(self, z):
        w = scipy.linalg.solve_triangular(self.dense, z, trans="T", lower=True)
        q = self.q_hat.copy()
        q[self.perm] += w
        return q

    def points(self, count=5, scale=0.4):
        return [scale * self.rng.randn(self.n) for _ in range(count)]


def wa_row(model, q):
    names = model._column_names()
    out = np.empty(len(names))
    rc = stanli._lib.stanli_wa_row(model._m, stanli._dptr(q), stanli._dptr(out))
    assert rc == 0
    return names, out


def check_lp_constant(fx):
    diffs = []
    for z in fx.points():
        lp_z, _ = fx.model.log_prob_grad(z)
        lp_q, _ = fx.inner.log_prob_grad(fx.q_of_z(z))
        scale = max(1.0, abs(lp_q))
        assert abs(lp_z - lp_q) <= 1e-10 * scale, (lp_z, lp_q)
        diffs.append(lp_z - lp_q)
    assert max(diffs) - min(diffs) <= 1e-10 * max(1.0, abs(lp_q)), diffs


def check_gradient(fx):
    for z in fx.points():
        _, g_z = fx.model.log_prob_grad(z)
        _, g_q = fx.inner.log_prob_grad(fx.q_of_z(z))
        want = scipy.linalg.solve_triangular(fx.dense, g_q[fx.perm],
                                             lower=True)
        scale = max(1.0, np.abs(want).max())
        assert np.abs(g_z - want).max() <= 1e-10 * scale, (g_z, want)
        h = 1e-6
        fd = np.empty(fx.n)
        for i in range(fx.n):
            e = np.zeros(fx.n)
            e[i] = h
            fd[i] = (fx.model.log_prob_grad(z + e)[0]
                     - fx.model.log_prob_grad(z - e)[0]) / (2 * h)
        assert np.abs(fd - g_z).max() <= 1e-6 * scale, (fd, g_z)


def check_rows(fx):
    for z in fx.points():
        names_w, row_w = wa_row(fx.model, z)
        names_i, row_i = wa_row(fx.inner, fx.q_of_z(z))
        assert names_w == names_i
        for name, a, b in zip(names_w, row_w, row_i):
            if name.startswith("y_rep"):
                assert np.isfinite(a) and np.isfinite(b), name
            else:
                assert abs(a - b) <= 1e-10 * abs(b) + 1e-12, (name, a, b)


def check_unconstrain(fx):
    n_con = len(fx.inner.constrained_names)
    for z in fx.points():
        names, row = wa_row(fx.model, z)
        values = {}
        for name, value in list(zip(names, row))[:n_con]:
            values.setdefault(name.split("[")[0], []).append(float(value))
        values = {k: (v[0] if len(v) == 1 else v) for k, v in values.items()}
        back = fx.model.unconstrain(values)
        assert np.allclose(back, z, rtol=0, atol=1e-9), (back, z)
        inner_q = fx.inner.unconstrain(values)
        assert np.allclose(inner_q, fx.q_of_z(z), rtol=0, atol=1e-9)


def check_hessian(fx):
    z = fx.points(1)[0]
    _, _, h_z = fx.model.log_prob_hessian(z)
    _, _, h_q = fx.inner.log_prob_hessian(fx.q_of_z(z))
    pq = h_q[np.ix_(fx.perm, fx.perm)]
    left = scipy.linalg.solve_triangular(fx.dense, pq, lower=True)
    want = scipy.linalg.solve_triangular(fx.dense, left.T, lower=True).T
    scale = max(1.0, np.abs(want).max())
    assert np.abs(h_z - want).max() <= 1e-5 * scale, np.abs(h_z - want).max()


def test_es_lp_is_constant():
    check_lp_constant(Fixture(es_model(), 1))


def test_radon_lp_is_constant():
    check_lp_constant(Fixture(radon_model(), 2))


def test_es_gradient():
    check_gradient(Fixture(es_model(), 3))


def test_radon_gradient():
    check_gradient(Fixture(radon_model(), 4))


def test_es_rows_match_inner():
    check_rows(Fixture(es_model(), 5))


def test_radon_rows_match_inner():
    check_rows(Fixture(radon_model(), 6))


def test_es_unconstrain_round_trip():
    check_unconstrain(Fixture(es_model(), 7))


def test_radon_unconstrain_round_trip():
    check_unconstrain(Fixture(radon_model(), 8))


def test_es_hessian():
    check_hessian(Fixture(es_model(), 9))


def test_radon_hessian():
    check_hessian(Fixture(radon_model(), 10))


def test_metadata_matches_inner():
    fx = Fixture(es_model(), 12)
    assert fx.model.n_unconstrained == fx.inner.n_unconstrained
    assert fx.model.constrained_names == fx.inner.constrained_names
    assert fx.model._column_names() == fx.inner._column_names()


def test_inner_survives_whitened_model():
    inner = es_model()
    fx = Fixture(inner, 13)
    z = fx.points(1)[0]
    lp, _ = fx.model.log_prob_grad(z)
    del fx
    lp_after, _ = inner.log_prob_grad(np.zeros(inner.n_unconstrained))
    assert np.isfinite(lp) and np.isfinite(lp_after)


def test_whitening_says_once_that_it_is_experimental():
    stanli._experimental_notified.clear()
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        Fixture(es_model(), 15)
        Fixture(es_model(), 16)
    notes = [w for w in caught
             if issubclass(w.category, stanli.ExperimentalWarning)]
    assert len(notes) == 1, [str(w.message) for w in caught]
    assert "Laplace whitening is experimental" in str(notes[0].message)


def test_csc_arrays_equal_scipy_matrix():
    fx = Fixture(es_model(), 14)
    again = fx.inner.whiten(fx.q_hat, fx.perm,
                            (fx.sparse.indptr, fx.sparse.indices,
                             fx.sparse.data))
    z = fx.points(1)[0]
    assert fx.model.log_prob_grad(z)[0] == again.log_prob_grad(z)[0]


def test_invalid_factors_are_rejected():
    inner = es_model()
    n = inner.n_unconstrained
    ident = np.arange(n)
    good = scipy.sparse.identity(n, format="csc")
    q_hat = np.zeros(n)

    def refused(*args, match):
        try:
            inner.whiten(*args)
        except (ValueError, RuntimeError) as e:
            assert match in str(e), str(e)
        else:
            raise AssertionError("expected whiten to refuse: " + match)

    upper = scipy.sparse.csc_matrix(np.triu(np.ones((n, n))))
    refused(q_hat, ident, upper, match="lower")
    refused(q_hat[:-1], ident, good, match="q_hat")
    refused(q_hat, np.zeros(n, dtype=np.int64), good, match="permutation")
    singular = scipy.sparse.csc_matrix(np.diag([0.0] + [1.0] * (n - 1)))
    refused(q_hat, ident, singular, match="diagonal")
    refused(q_hat, ident, scipy.sparse.identity(n + 1, format="csc"),
            match="L")


def laplace_factor(inner):
    mode = inner.optimize(seed=3)
    q_hat = np.array(mode.unconstrained)
    _, _, h = inner.log_prob_hessian(q_hat)
    w, v = np.linalg.eigh(-(h + h.T) / 2)
    w = np.maximum(w, 1e-3 * max(1.0, w.max()))
    precision = (v * w) @ v.T
    return q_hat, scipy.sparse.csc_matrix(np.linalg.cholesky(precision))


def check_posterior_means(inner):
    q_hat, chol = laplace_factor(inner)
    whitened = inner.whiten(q_hat, np.arange(inner.n_unconstrained), chol)
    reference = inner.sample(chains=4, seed=21, warmup=1000, samples=1000,
                             refresh=0)
    fit = whitened.sample(chains=4, seed=22, warmup=200, samples=1000,
                          metric="unit_e", adapt_metric=False, refresh=0)
    assert fit.names == reference.names
    ref_summary = reference.summary()
    new_summary = fit.summary()
    worst = 0.0
    for name in fit.names[:len(inner.constrained_names)]:
        a, b = ref_summary[name], new_summary[name]
        mcse = np.hypot(a["mcse_mean"], b["mcse_mean"])
        worst = max(worst, abs(a["mean"] - b["mean"]) / mcse)
    print(f"  worst posterior-mean gap: {worst:.2f} MCSE")
    assert worst <= 4.0, worst


def test_es_sampling_matches_default_sampler():
    check_posterior_means(es_model())


def test_radon_sampling_matches_default_sampler():
    check_posterior_means(radon_model())


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
