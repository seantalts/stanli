#!/usr/bin/env python3
"""Tests for the sparse pieces of stanli.laplace_whiten: the sparse LDL^T
factor, the diagonal-plus-low-rank whitening, the coloured Hessian and the
builder options built on them.

Needs numpy and scipy. Run against an installed wheel or a local build:

    PYTHONPATH=python python3 tests/test_sparse_laplace.py
"""
import pathlib
import sys
import warnings

import numpy as np
import scipy.linalg
import scipy.sparse

import stanli
from stanli import laplace

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


def fixing_correction(Q):
    w, V = np.linalg.eigh(Q)
    bad = w < 1e-8 * w.max()
    delta = np.maximum(np.abs(w[bad]), 1e-8 * np.abs(w).max()) - w[bad]
    return V[:, bad] * np.sqrt(delta), w


def lower_csc(A):
    return scipy.sparse.csc_matrix(np.tril(A))


def test_ldlt_reproduces_matrix_with_amd_and_given_orders():
    rng = np.random.RandomState(1)
    n = 40
    S = scipy.sparse.random(n, n, density=0.08, random_state=2).toarray()
    A = (S + S.T) * 0.4 + np.diag(rng.uniform(2.5, 3.5, n))
    assert np.linalg.eigvalsh(A).min() > 0
    for perm in (None, rng.permutation(n), np.arange(n)):
        f = laplace._ldlt(lower_csc(A), perm)
        P = A[np.ix_(f.perm, f.perm)]
        L = f.L.toarray()
        assert np.allclose(np.diag(L), 1) and np.allclose(np.triu(L, 1), 0)
        assert np.abs(L @ np.diag(f.d) @ L.T - P).max() <= 1e-12 * 4, \
            np.abs(L @ np.diag(f.d) @ L.T - P).max()
        assert f.n_negative == 0
        assert abs(f.logabsdet - np.linalg.slogdet(A)[1]) <= 1e-10
        assert f.nnz == f.L.nnz
        if perm is not None:
            assert list(f.perm) == list(perm)
        else:
            assert sorted(f.perm) == list(range(n))


def test_ldlt_counts_negative_pivots_of_an_indefinite_matrix():
    rng = np.random.RandomState(3)
    A = banded(30, rng, negative=(4, 17))
    f = laplace._ldlt(lower_csc(A), None)
    assert f.n_negative == int((np.linalg.eigvalsh(A) < 0).sum())
    assert abs(f.logabsdet - np.linalg.slogdet(A)[1]) <= 1e-9


def test_ldlt_reports_a_zero_pivot():
    A = np.array([[0.0, 1.0], [1.0, 0.0]])
    try:
        laplace._ldlt(lower_csc(A), np.arange(2))
    except RuntimeError as e:
        assert "pivot" in str(e), str(e)
    else:
        raise AssertionError("expected a zero pivot")


def test_ldlt_refuses_a_bad_permutation():
    A = banded(5, np.random.RandomState(0))
    try:
        laplace._ldlt(lower_csc(A), np.array([0, 1, 2, 2, 4]))
    except ValueError as e:
        assert "permutation" in str(e), str(e)
    else:
        raise AssertionError("expected a refusal")


def whitened_gaussian(Qp, factor, lowrank):
    n = Qp.shape[0]
    model = stanli.Model(stan_code=GAUSSIAN, data={"n": n,
                                                   "A": Qp.tolist()})
    q_hat = np.linspace(-0.3, 0.4, n)
    return model, model.whiten(q_hat, factor.perm, factor.L, lowrank=lowrank)


def check_implied_precision(Q, U, perm):
    n = Q.shape[0]
    Qp = Q + U @ U.T
    assert np.linalg.eigvalsh(Qp).min() > 0
    f = laplace._ldlt(lower_csc(Q), perm)
    lowrank = f.lowrank(U)
    model, whitened = whitened_gaussian(Qp, f, lowrank)
    rng = np.random.RandomState(7)
    z = 0.5 * rng.randn(n)
    _, grad, hess = whitened.log_prob_hessian(z)
    assert np.abs(-hess - np.eye(n)).max() <= 1e-7, np.abs(-hess - np.eye(n)).max()
    lp, g = whitened.log_prob_grad(z)
    for i in range(n):
        e = np.zeros(n)
        e[i] = 1e-6
        fd = (whitened.log_prob_grad(z + e)[0]
              - whitened.log_prob_grad(z - e)[0]) / 2e-6
        assert abs(fd - g[i]) <= 1e-7 * max(1, abs(g[i])), (i, fd, g[i])
    c, B, W = lowrank
    C = np.diag(c) + np.tril(W @ B.T, -1)
    T = np.diag(f.d) + W @ W.T
    assert np.abs(C @ C.T - T).max() <= 1e-10 * np.abs(T).max()


def test_lowrank_whitening_implies_the_corrected_precision():
    rng = np.random.RandomState(4)
    Q = banded(24, rng, negative=(5, 13))
    U, w = fixing_correction(Q)
    assert U.shape[1] == 2
    for perm in (None, np.arange(24)[::-1].copy()):
        check_implied_precision(Q, U, perm)


def test_bad_eigenpairs_with_a_wide_spectrum():
    rng = np.random.RandomState(3)
    n = 144
    w = np.concatenate([[-1.1, -0.5, 0.2, 0.3, 0.35, 0.43, 0.57, 0.64, 0.67],
                        np.geomspace(1.9, 5e5, n - 9)])
    V, _ = np.linalg.qr(rng.randn(n, n))
    Q = scipy.sparse.csc_matrix((V * w) @ V.T)
    bad, vecs, lo, hi = laplace._bad_eigenpairs(Q, 1e-8, 2)
    assert np.allclose(np.sort(bad), [-1.1, -0.5], rtol=1e-8)
    assert abs(lo + 1.1) < 1e-8 and abs(hi - 5e5) < 1e-3
    assert np.allclose(Q @ vecs, vecs * bad, atol=1e-6)


def test_lowrank_with_one_column_and_positive_definite_base():
    rng = np.random.RandomState(5)
    Q = banded(16, rng)
    U = 0.7 * rng.randn(16, 1)
    check_implied_precision(Q, U, None)


def test_lowrank_not_positive_definite_is_refused():
    rng = np.random.RandomState(6)
    Q = banded(12, rng, negative=(3,))
    f = laplace._ldlt(lower_csc(Q), None)
    U = 0.01 * np.ones((12, 1))
    try:
        f.lowrank(U)
    except RuntimeError as e:
        assert "positive definite" in str(e), str(e)
    else:
        raise AssertionError("expected a refusal")


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
        names = inner._column_names()[0]
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
    names = inner._column_names()[0]
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


def test_distance_two_colouring_is_valid_and_small_for_a_band():
    rng = np.random.RandomState(8)
    n = 60
    S = scipy.sparse.random(n, n, density=0.04, random_state=3).toarray() != 0
    pattern = scipy.sparse.csc_matrix(S | S.T | np.eye(n, dtype=bool))
    color = laplace._distance2_colouring(pattern)
    dense = pattern.toarray()
    for c in np.unique(color):
        cols = np.nonzero(color == c)[0]
        assert dense[:, cols].sum(1).max() <= 1, c
    band = scipy.sparse.diags([np.ones(n - 1), np.ones(n), np.ones(n - 1)],
                              [-1, 0, 1], format="csc")
    assert laplace._distance2_colouring(band).max() + 1 == 3


def grad_of(model):
    count = [0]

    def grad(q):
        count[0] += 1
        return model.log_prob_grad(q)
    return grad, count


def test_sparse_hessian_matches_dense_on_a_hierarchical_model():
    m = stanli.Model(stan_code=CENTRED, data=radon_data(J=14, N=140, seed=5))
    n = m.n_unconstrained
    rng = np.random.RandomState(0)
    q = 0.3 * rng.randn(n)
    _, _, H = m.log_prob_hessian(q)
    pattern = laplace._pattern(H, 1e-6)
    it = np.setdiff1d(np.arange(n), np.arange(2, 16))
    grad, count = grad_of(m)
    Hs = laplace._sparse_hessian(grad, q, pattern, np.arange(n), dense=it)
    assert scipy.sparse.issparse(Hs)
    err = np.abs(Hs.toarray() - H).max() / np.abs(H).max()
    print(f"  sparse vs dense Hessian, relative max error {err:.2e}, "
          f"{count[0]} gradients vs {2 * n}")
    assert err <= 1e-6, err
    assert count[0] < 2 * n


def test_sparse_hessian_over_a_block_uses_only_that_block():
    m = stanli.Model(stan_code=CENTRED, data=radon_data(J=14, N=140, seed=5))
    n = m.n_unconstrained
    q = 0.3 * np.random.RandomState(1).randn(n)
    _, _, H = m.log_prob_hessian(q)
    iu = np.arange(2, 16)
    pattern = laplace._pattern(H, 1e-6)[iu][:, iu]
    grad, count = grad_of(m)
    Hs = laplace._sparse_hessian(grad, q, pattern, iu, dense=())
    err = np.abs(Hs.toarray() - H[np.ix_(iu, iu)]).max() / np.abs(H).max()
    assert err <= 1e-6, err
    assert count[0] == 2 * (laplace._distance2_colouring(pattern).max() + 1)


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
