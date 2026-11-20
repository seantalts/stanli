#!/usr/bin/env python3
"""Tests for the sparse parts of stanli.laplace_whiten as a user meets them:
the diagonal-plus-low-rank whitening and the builder options. The builder's
pieces (the sparse LDL^T factor, the colouring, the coloured Hessian, the
eigenvalue search) are tested in C++, in tests/test_laplace.cpp.

Needs numpy and scipy. Run against an installed wheel or a local build:

    PYTHONPATH=python python3 tests/test_sparse_laplace.py
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

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_whiten import es_model, radon_data, radon_model  # noqa: E402
from test_laplace_whiten import CENTRED, SADDLE  # noqa: E402

GAUSSIAN = """
data {
  int<lower=1> n;
  matrix[n, n] A;
}
parameters {
  vector[n] q;
}
model {
  target += -0.5 * dot_product(q, A * q);
}
"""

SADDLES = """
data {
  int<lower=1> J;
}
parameters {
  vector[J] x;
  vector[J] y;
}
model {
  target += sum(square(x)) / 2 - sum(square(square(x))) / 4;
  y ~ normal(0.5 * x, 1);
}
"""


def banded(n, rng, negative=()):
    d = rng.uniform(2.0, 3.0, n)
    for i in negative:
        d[i] = -rng.uniform(0.5, 1.0)
    off = -rng.uniform(0.2, 0.6, n - 1)
    return np.diag(d) + np.diag(off, 1) + np.diag(off, -1)


def check_repaired_gaussian(A):
    """laplace_whiten on the Gaussian with the indefinite precision A: the
    repaired precision has the absolute eigenvalues of A, and the whitened
    Hessian, Q'^-1/2 A Q'^-1/2 in some basis, squares to the identity."""
    n = A.shape[0]
    model = stanli.Model(stan_code=GAUSSIAN, data={"n": n, "A": A.tolist()})
    wh, rep, caught = build(model, latent=["q"], init=np.zeros(n),
                            keep_precision=True)
    w, V = np.linalg.eigh(A)
    want = (V * np.maximum(np.abs(w), 1e-8 * np.abs(w).max())) @ V.T
    assert rep.n_clipped == int((w < 0).sum()) and rep.rank == rep.n_clipped
    assert any("absolute" in str(c.message) for c in caught)
    assert np.abs(rep.precision - want).max() <= 1e-10 * np.abs(want).max(), \
        np.abs(rep.precision - want).max()
    _, _, h = wh.log_prob_hessian(np.zeros(n))
    assert np.abs(h @ h - np.eye(n)).max() <= 1e-10, np.abs(h @ h - np.eye(n)).max()
    assert int((np.linalg.eigvalsh(-h) < 0).sum()) == rep.n_clipped


def test_lowrank_whitening_implies_the_repaired_precision():
    rng = np.random.RandomState(4)
    check_repaired_gaussian(banded(24, rng, negative=(5, 13)))
    check_repaired_gaussian(banded(90, rng, negative=(7, 40, 71)))


def test_lowrank_validates_shapes():
    m = es_model()
    n = m.n_unconstrained
    L = scipy.sparse.identity(n, format="csc")
    for bad in ((np.ones(n), np.ones((n, 2)), np.ones((n, 3))),
                (np.ones(n + 1), np.ones((n, 1)), np.ones((n, 1))),
                (np.ones(n), np.ones(n), np.ones(n))):
        try:
            m.whiten(np.zeros(n), np.arange(n), L, lowrank=bad)
        except ValueError as e:
            assert "lowrank" in str(e), str(e)
        else:
            raise AssertionError("expected a refusal")
    c = np.ones(n)
    c[2] = 0.0
    try:
        m.whiten(np.zeros(n), np.arange(n), L,
                 lowrank=(c, np.ones((n, 1)), np.ones((n, 1))))
    except ValueError as e:
        assert "zero" in str(e), str(e)
    else:
        raise AssertionError("expected a refusal")


def check_kernel_against_dense_c(inner, seed):
    rng = np.random.RandomState(seed)
    n = inner.n_unconstrained
    k = 3
    q_hat = 0.2 * rng.randn(n)
    perm = rng.permutation(n)
    L = np.tril(rng.randn(n, n) * 0.2 * (rng.rand(n, n) < 0.3), -1)
    L += np.diag(rng.uniform(0.8, 1.6, n))
    c = rng.uniform(0.8, 1.5, n)
    B = 0.3 * rng.randn(n, k)
    W = 0.3 * rng.randn(n, k)
    C = np.diag(c) + np.tril(W @ B.T, -1)
    whitened = inner.whiten(q_hat, perm, scipy.sparse.csc_matrix(L),
                            lowrank=(c, B, W))

    def q_of_z(z):
        y = scipy.linalg.solve_triangular(C, z, trans="T", lower=True)
        w = scipy.linalg.solve_triangular(L, y, trans="T", lower=True)
        q = q_hat.copy()
        q[perm] += w
        return q

    for _ in range(4):
        z = 0.4 * rng.randn(n)
        lp_z, g_z = whitened.log_prob_grad(z)
        lp_q, g_q = inner.log_prob_grad(q_of_z(z))
        assert abs(lp_z - lp_q) <= 1e-10 * max(1, abs(lp_q)), (lp_z, lp_q)
        want = scipy.linalg.solve_triangular(
            C, scipy.linalg.solve_triangular(L, g_q[perm], lower=True),
            lower=True)
        assert np.abs(g_z - want).max() <= 1e-10 * max(1, np.abs(want).max())
        names = inner._column_names()
        row = np.empty(len(names))
        stanli._lib.stanli_wa_row(whitened._m, stanli._dptr(z),
                                  stanli._dptr(row))
        row_i = np.empty(len(names))
        stanli._lib.stanli_wa_row(inner._m, stanli._dptr(q_of_z(z)),
                                  stanli._dptr(row_i))
        keep = [i for i, nm in enumerate(names) if not nm.startswith("y_rep")]
        assert np.abs(row[keep] - row_i[keep]).max() <= 1e-10 * max(
            1, np.abs(row_i[keep]).max())


def test_kernel_with_c_matches_dense_c_on_eight_schools():
    check_kernel_against_dense_c(es_model(), 1)


def test_kernel_with_c_matches_dense_c_on_radon():
    check_kernel_against_dense_c(radon_model(), 2)


def test_unconstrain_round_trip_with_c():
    inner = es_model()
    rng = np.random.RandomState(9)
    n = inner.n_unconstrained
    c = rng.uniform(0.8, 1.5, n)
    B = 0.3 * rng.randn(n, 2)
    W = 0.3 * rng.randn(n, 2)
    whitened = inner.whiten(0.1 * rng.randn(n), rng.permutation(n),
                            scipy.sparse.identity(n, format="csc"),
                            lowrank=(c, B, W))
    names = inner._column_names()
    n_con = len(inner.constrained_names)
    for _ in range(3):
        z = 0.4 * rng.randn(n)
        row = np.empty(len(names))
        stanli._lib.stanli_wa_row(whitened._m, stanli._dptr(z),
                                  stanli._dptr(row))
        values = {}
        for name, value in list(zip(names, row))[:n_con]:
            values.setdefault(name.split("[")[0], []).append(float(value))
        values = {k: (v[0] if len(v) == 1 else v) for k, v in values.items()}
        assert np.allclose(whitened.unconstrain(values), z, atol=1e-9)


def build(model, **kw):
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        out = stanli.laplace_whiten(model, **kw)
    return out + (caught,)


def test_hessian_option_dense_and_coloured_agree():
    m = stanli.Model(stan_code=CENTRED, data=radon_data(J=30, N=300, seed=6))
    n = m.n_unconstrained
    _, dense, _ = build(m, latent=["alpha"], seed=1, hessian="dense",
                        keep_precision=True)
    _, col, _ = build(m, latent=["alpha"], seed=1, hessian="coloured",
                      keep_precision=True)
    err = np.abs(dense.precision - col.precision).max() / np.abs(
        dense.precision).max()
    print(f"  precision relative difference {err:.2e}; final-Hessian "
          f"gradients {dense.gradients['final_hessian']} dense vs "
          f"{col.gradients['final_hessian']} coloured (n={n})")
    assert err <= 1e-6, err
    assert dense.gradients["final_hessian"] == 1 + 2 * n
    assert col.gradients["final_hessian"] < 1 + 2 * n


def test_hessian_option_unknown_is_refused():
    m = stanli.Model(stan_code=SADDLE)
    try:
        stanli.laplace_whiten(m, latent=["y"], init=np.zeros(2),
                              hessian="sparse")
    except ValueError as e:
        assert "hessian" in str(e), str(e)
    else:
        raise AssertionError("expected a refusal")


def test_sparse_builder_matches_dense_builder_precision():
    m = stanli.Model(stan_code=CENTRED, data=radon_data(J=30, N=300, seed=6))
    n = m.n_unconstrained
    _, dense, _ = build(m, latent=["alpha"], seed=1, repair="abs_dense",
                        keep_precision=True)
    wh, sparse, _ = build(m, latent=["alpha"], seed=1, repair="abs",
                          keep_precision=True)
    assert np.abs(sparse.q_hat - dense.q_hat).max() <= 1e-6
    err = np.abs(dense.precision - sparse.precision).max() / np.abs(
        dense.precision).max()
    assert err <= 1e-6, err
    assert sparse.ordering == "amd" and sparse.nnz_L <= 2 * dense.nnz_L
    z = np.zeros(n)
    _, _, h = wh.log_prob_hessian(z)
    assert np.abs(-h - np.eye(n)).max() <= 1e-3


def test_repair_on_many_saddles_matches_the_dense_repair():
    J = 14
    m = stanli.Model(stan_code=SADDLES, data={"J": J})
    init = np.zeros(2 * J)
    _, dense, cd = build(m, latent=["y"], init=init, repair="abs_dense",
                         keep_precision=True)
    wh, sparse, cs = build(m, latent=["y"], init=init, repair="abs",
                           keep_precision=True)
    assert dense.n_clipped == J and sparse.n_clipped == J, (
        dense.n_clipped, sparse.n_clipped)
    assert any("absolute" in str(w.message) for w in cs)
    err = np.abs(dense.precision - sparse.precision).max() / np.abs(
        dense.precision).max()
    assert err <= 1e-6, err
    assert abs(sparse.min_eigenvalue - dense.min_eigenvalue) <= 1e-6
    lp, g = wh.log_prob_grad(np.array([0.1, -0.2] * J))
    assert np.isfinite(lp) and np.all(np.isfinite(g))
    fit = wh.sample(chains=2, warmup=100, samples=100, seed=1,
                    metric="unit_e", adapt_metric=False, refresh=0)
    assert np.all(np.isfinite(fit.draws()))


def test_saddle_repair_matches_the_dense_repair():
    m = stanli.Model(stan_code=SADDLE)
    _, dense, _ = build(m, latent=["y"], init=np.zeros(2),
                        repair="abs_dense", keep_precision=True)
    _, sparse, _ = build(m, latent=["y"], init=np.zeros(2), repair="abs",
                         keep_precision=True)
    assert np.abs(dense.precision - sparse.precision).max() <= 1e-8
    assert sparse.n_clipped == dense.n_clipped == 1


def test_clip_repair_still_works_through_the_sparse_builder():
    m = stanli.Model(stan_code=SADDLE)
    wh, rep, caught = build(m, latent=["y"], init=np.zeros(2), repair="clip",
                            keep_precision=True)
    assert rep.n_clipped >= 1
    assert any("clipped" in str(w.message) for w in caught)
    assert np.linalg.eigvalsh(rep.precision).min() > 0


def test_unknown_repair_lists_the_choices():
    m = stanli.Model(stan_code=SADDLE)
    try:
        stanli.laplace_whiten(m, latent=["y"], init=np.zeros(2), repair="soft")
    except ValueError as e:
        assert "abs_dense" in str(e), str(e)
    else:
        raise AssertionError("expected a refusal")


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
