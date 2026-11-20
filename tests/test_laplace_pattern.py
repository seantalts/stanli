#!/usr/bin/env python3
"""The `pattern` option of laplace_whiten: the sparsity pattern the builder
starts from, probed from gradient differences (the default) or taken from
the dense Hessian. The probe itself, its verification and its fallback are
tested in C++, in tests/test_laplace.cpp and tests/test_laplace_capi.cpp.

Needs numpy and scipy. Run against an installed wheel or a local build:

    PYTHONPATH=python python3 tests/test_laplace_pattern.py
"""
import pathlib
import sys

try:
    import numpy as np
except ImportError as exc:
    print(f"SKIP {pathlib.Path(__file__).name}: needs numpy and scipy ({exc})")
    sys.exit(0)

import stanli

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_whiten import radon_data  # noqa: E402
from test_laplace_whiten import CENTRED  # noqa: E402


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
