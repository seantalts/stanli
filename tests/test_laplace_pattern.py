#!/usr/bin/env python3
"""The sparsity pattern the laplace_whiten builder starts from.

The default probe finds the pattern from gradient differences along random
group directions, in O(1) gradients per group and without forming the dense
Hessian. These tests require it to equal the pattern of the dense Hessian
(no entry missed), to scale, and to fall back to the dense pattern when its
own verification fails.

Needs numpy and scipy. Run against an installed wheel or a local build:

    PYTHONPATH=python python3 tests/test_laplace_pattern.py
"""
import json
import os
import pathlib
import sys
import warnings

try:
    import numpy as np
    import scipy.sparse
except ImportError as exc:
    print(f"SKIP {pathlib.Path(__file__).name}: needs numpy and scipy ({exc})")
    sys.exit(0)

import stanli
from stanli import laplace

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_whiten import radon_data  # noqa: E402
from test_laplace_whiten import CENTRED  # noqa: E402

FINDINGS = pathlib.Path(os.environ.get(
    "STANLI_BRMS_FINDINGS",
    "/Users/xitrium/claud/stanli-issue540-findings/precond2/brms"))

TOL = 1e-6

HUB = """
data { int<lower=2> J; }
parameters { real mu; vector[J] a; }
model {
  a ~ normal(mu, 1);
  mu ~ normal(0, 3);
  target += -0.5 * square(sum(a)) / J;
}
"""


def brms(name):
    stan = (HERE / "brms" / f"{name}.stan").read_text()
    data = json.loads((HERE / "brms" / f"{name}.json").read_text())
    return stanli.Model(stan_code=stan, data=data)


def findings_model(stan_name, data_name):
    path = FINDINGS / stan_name
    if not path.exists():
        return None
    return stanli.Model(
        stan_code=path.read_text(),
        data=json.loads((FINDINGS / data_name).read_text()))


def dense_pattern(model, q, iu):
    _, _, H = model.log_prob_hessian(q)
    return laplace._pattern(H, TOL)[iu][:, iu]


def same(a, b):
    a = scipy.sparse.csc_matrix(a, dtype=bool)
    b = scipy.sparse.csc_matrix(b, dtype=bool)
    return (a != b).nnz == 0


def check_probe_equals_dense(model, latent, seed=3, scale=0.3):
    n = model.n_unconstrained
    q = scale * np.random.RandomState(seed).randn(n)
    iu = laplace._block_index(model, latent)
    want = dense_pattern(model, q, iu)
    count = [0]
    got = laplace._probe_pattern(model, q, iu, TOL, count, seed=1)
    assert got is not None, "probe verification failed"
    assert scipy.sparse.issparse(got) and got.shape == want.shape
    missed = (scipy.sparse.csc_matrix(want, dtype=np.int8)
              - scipy.sparse.csc_matrix(got, dtype=np.int8).multiply(want))
    assert missed.nnz == 0, f"{missed.nnz} nonzeros of the dense pattern missed"
    assert same(got, want), (got.nnz, want.nnz)
    return count[0], n, got.nnz


def test_radon_pattern_equals_dense():
    m = stanli.Model(stan_code=CENTRED, data=radon_data(J=14, N=140, seed=5))
    check_probe_equals_dense(m, ["alpha"])


def test_brms_random_slopes_pattern_equals_dense():
    check_probe_equals_dense(brms("sw_re_slope"), ["z_1"])


def test_brms_multimembership_pattern_equals_dense():
    check_probe_equals_dense(brms("s2_mm"), ["z_1"])


def test_brms_poisson_pattern_equals_dense():
    check_probe_equals_dense(brms("i319_pois_re"), ["z_1"])


def test_brms_crossed_effects_pattern_equals_dense():
    m = findings_model("crossed.stan", "crossed.json")
    if m is None:
        print("skip: no findings directory")
        return
    check_probe_equals_dense(m, ["z_1", "z_2"])


def test_centred_slopes_pattern_equals_dense_and_is_cheap():
    m = findings_model("gauss_slopes_c.stan", "gauss_slopes.json")
    if m is None:
        print("skip: no findings directory")
        return
    grads, n, _ = check_probe_equals_dense(m, ["r_1"])
    assert grads < (1 + 2 * n) // 4, (grads, n)


def test_pattern_gradients_do_not_grow_with_groups():
    counts = []
    for groups in (300, 1200):
        m = stanli.Model(stan_code=CENTRED,
                         data=radon_data(J=groups, N=10 * groups, seed=5))
        q = 0.3 * np.random.RandomState(2).randn(m.n_unconstrained)
        count = [0]
        iu = laplace._block_index(m, ["alpha"])
        got = laplace._probe_pattern(m, q, iu, TOL, count, seed=1)
        assert got is not None
        counts.append(count[0])
    assert counts[1] <= counts[0] + 20, counts
    assert counts[1] < (1 + 2 * 1200) // 8, counts


def test_dense_hub_row_is_found():
    m = stanli.Model(stan_code=HUB, data={"J": 40})
    check_probe_equals_dense(m, ["a"])


def test_failed_verification_falls_back_to_the_dense_pattern():
    m = stanli.Model(stan_code=HUB, data={"J": 40})
    n = m.n_unconstrained
    q = 0.3 * np.random.RandomState(3).randn(n)
    iu = laplace._block_index(m, ["a"])
    want = dense_pattern(m, q, iu)

    def lossy(*args, **kwargs):
        size = len(iu)
        return scipy.sparse.identity(size, dtype=bool, format="csc")

    real = laplace._probe_candidates
    laplace._probe_candidates = lossy
    try:
        count = [0]
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            got = laplace._start_pattern(m, q, iu, TOL, "probe", 1, count)
    finally:
        laplace._probe_candidates = real
    assert same(got, want)
    assert any("dense" in str(c.message) for c in caught), \
        [str(c.message) for c in caught]
    assert count[0] >= 1 + 2 * n


def test_verification_catches_one_missed_entry():
    m = brms("s2_mm")
    q = 0.3 * np.random.RandomState(3).randn(m.n_unconstrained)
    iu = laplace._block_index(m, ["z_1"])
    want = dense_pattern(m, q, iu)
    upper = scipy.sparse.triu(want, k=1).tocoo()
    assert upper.nnz > 0
    pairs = [(upper.row[k], upper.col[k]) for k in range(0, upper.nnz,
                                                         max(1, upper.nnz // 6))]
    real = laplace._probe_candidates
    caught = 0
    for i, j in pairs:
        def missing(*args, _i=i, _j=j, **kwargs):
            C = scipy.sparse.lil_matrix(real(*args, **kwargs))
            C[_i, _j] = False
            C[_j, _i] = False
            return scipy.sparse.csc_matrix(C)

        laplace._probe_candidates = missing
        try:
            got = laplace._probe_pattern(m, q, iu, TOL, [0], seed=1)
        finally:
            laplace._probe_candidates = real
        caught += got is None
    assert caught == len(pairs), (caught, len(pairs))


def test_pattern_option_dense_gives_the_dense_pattern():
    m = stanli.Model(stan_code=CENTRED, data=radon_data(J=14, N=140, seed=5))
    n = m.n_unconstrained
    _, rep = stanli.laplace_whiten(m, latent=["alpha"], seed=1,
                                   pattern="dense")
    assert rep.gradients["pattern_hessian"] == 1 + 2 * n


def test_pattern_option_probe_is_the_default_and_cheaper():
    m = stanli.Model(stan_code=CENTRED, data=radon_data(J=60, N=600, seed=5))
    n = m.n_unconstrained
    _, rep_a = stanli.laplace_whiten(m, latent=["alpha"], seed=1)
    _, rep_b = stanli.laplace_whiten(m, latent=["alpha"], seed=1,
                                     pattern="dense")
    assert rep_a.gradients["pattern_hessian"] < 1 + 2 * n
    assert np.allclose(rep_a.q_hat, rep_b.q_hat, rtol=0, atol=1e-8)
    assert np.allclose(rep_a.theta_hat, rep_b.theta_hat, rtol=0, atol=1e-8)
    assert rep_a.nnz_L == rep_b.nnz_L


def test_unknown_pattern_is_refused():
    m = stanli.Model(stan_code=CENTRED, data=radon_data(J=14, N=140, seed=5))
    try:
        stanli.laplace_whiten(m, latent=["alpha"], pattern="exact")
    except ValueError as e:
        assert "dense" in str(e) and "probe" in str(e), str(e)
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
