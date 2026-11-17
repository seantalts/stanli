#!/usr/bin/env python3
"""A whitened Model served through the BridgeStan C ABI.

``Model.bridgestan_data()`` on a whitened model carries the whitening in the
manifest. The tests drive the shipped library through ctypes (no bridgestan
package needed) and require the density, gradient, constrain and unconstrain
maps to be bitwise those of the in-process whitened model. When the
bridgestan package is installed, a short HMC run through it is compared with
stanli's own NUTS on the same whitened model.

Needs numpy and scipy. Run against an installed wheel or a local build:

    PYTHONPATH=python:tests python3 tests/test_bridgestan_whiten.py
"""
import base64
import ctypes
import json
import pathlib
import sys

try:
    import numpy as np
    import scipy.sparse
except ImportError as exc:
    print(f"SKIP {pathlib.Path(__file__).name}: needs numpy and scipy ({exc})")
    sys.exit(0)

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import stanli
from test_whiten import Fixture, es_model, radon_model, random_factor

_dp = ctypes.POINTER(ctypes.c_double)
_cp = ctypes.POINTER(ctypes.c_char_p)


def bind():
    lib = ctypes.CDLL(str(stanli.runtime_path()))
    lib.bs_model_construct.restype = ctypes.c_void_p
    lib.bs_model_construct.argtypes = [ctypes.c_char_p, ctypes.c_uint, _cp]
    lib.bs_model_destruct.argtypes = [ctypes.c_void_p]
    lib.bs_param_unc_num.restype = ctypes.c_int
    lib.bs_param_unc_num.argtypes = [ctypes.c_void_p]
    lib.bs_param_unc_names.restype = ctypes.c_char_p
    lib.bs_param_unc_names.argtypes = [ctypes.c_void_p]
    lib.bs_param_num.restype = ctypes.c_int
    lib.bs_param_num.argtypes = [ctypes.c_void_p, ctypes.c_bool, ctypes.c_bool]
    lib.bs_log_density_gradient.restype = ctypes.c_int
    lib.bs_log_density_gradient.argtypes = [
        ctypes.c_void_p, ctypes.c_bool, ctypes.c_bool, _dp, _dp, _dp, _cp]
    lib.bs_param_constrain.restype = ctypes.c_int
    lib.bs_param_constrain.argtypes = [
        ctypes.c_void_p, ctypes.c_bool, ctypes.c_bool, _dp, _dp,
        ctypes.c_void_p, _cp]
    lib.bs_param_unconstrain.restype = ctypes.c_int
    lib.bs_param_unconstrain.argtypes = [ctypes.c_void_p, _dp, _dp, _cp]
    lib.bs_param_unconstrain_json.restype = ctypes.c_int
    lib.bs_param_unconstrain_json.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                              _dp, _cp]
    return lib


def construct(lib, text):
    err = ctypes.c_char_p()
    m = lib.bs_model_construct(text.encode(), 1234, ctypes.byref(err))
    if not m:
        raise RuntimeError(err.value.decode() if err.value else "(no message)")
    return m


def ok(rc, err):
    if rc != 0:
        raise RuntimeError(err.value.decode() if err.value else "(no message)")


def bs_grad(lib, m, z):
    z = np.ascontiguousarray(z, dtype=np.float64)
    lp = ctypes.c_double()
    g = np.empty(z.size)
    err = ctypes.c_char_p()
    ok(lib.bs_log_density_gradient(m, True, True, z.ctypes.data_as(_dp),
                                   ctypes.byref(lp), g.ctypes.data_as(_dp),
                                   ctypes.byref(err)), err)
    return lp.value, g


def bs_constrain(lib, m, z, tp):
    z = np.ascontiguousarray(z, dtype=np.float64)
    out = np.empty(lib.bs_param_num(m, tp, False))
    err = ctypes.c_char_p()
    ok(lib.bs_param_constrain(m, tp, False, z.ctypes.data_as(_dp),
                              out.ctypes.data_as(_dp), None,
                              ctypes.byref(err)), err)
    return out


def bs_unconstrain(lib, m, theta, n):
    theta = np.ascontiguousarray(theta, dtype=np.float64)
    out = np.empty(n)
    err = ctypes.c_char_p()
    ok(lib.bs_param_unconstrain(m, theta.ctypes.data_as(_dp),
                                out.ctypes.data_as(_dp), ctypes.byref(err)),
       err)
    return out


def bs_unconstrain_json(lib, m, values, n):
    out = np.empty(n)
    err = ctypes.c_char_p()
    ok(lib.bs_param_unconstrain_json(m, json.dumps(values).encode(),
                                     out.ctypes.data_as(_dp),
                                     ctypes.byref(err)), err)
    return out


def inner_row(model, z, width):
    names, _ = model._column_names()
    out = np.empty(len(names))
    rc = stanli._lib.stanli_wa_row(model._m, stanli._dptr(z), stanli._dptr(out))
    assert rc == 0
    return names, out[:width]


def check_bitwise(label, whitened, points):
    lib = bind()
    m = construct(lib, whitened.bridgestan_data(name="whitened"))
    try:
        n = whitened.n_unconstrained
        assert lib.bs_param_unc_num(m) == n
        assert lib.bs_param_unc_names(m).decode() == ",".join(
            f"z.{i}" for i in range(1, n + 1))
        n_con = len(whitened.constrained_names)
        n_tp = lib.bs_param_num(m, True, False) - n_con
        for z in points:
            lp, g = whitened.log_prob_grad(z)
            got_lp, got_g = bs_grad(lib, m, z)
            assert got_lp == lp, (label, got_lp, lp)
            assert np.array_equal(got_g, g), (label, got_g, g)

            names, row = inner_row(whitened, z, n_con + n_tp)
            for tp in (False, True):
                got = bs_constrain(lib, m, z, tp)
                want = row[:n_con + (n_tp if tp else 0)]
                assert np.array_equal(got, want), (label, tp, got, want)

            theta = row[:n_con]
            back = bs_unconstrain(lib, m, theta, n)
            values = {}
            for name, value in zip(names[:n_con], theta):
                values.setdefault(name.split("[")[0], []).append(float(value))
            values = {k: (v[0] if len(v) == 1 else v)
                      for k, v in values.items()}
            want_z = whitened.unconstrain(values)
            assert np.array_equal(back, want_z), (label, back, want_z)
            assert np.array_equal(bs_unconstrain_json(lib, m, values, n),
                                  want_z), label
    finally:
        lib.bs_model_destruct(m)


def lowrank_for(n, rng, k=2):
    return (rng.uniform(0.8, 1.4, n), 0.3 * rng.randn(n, k),
            0.3 * rng.randn(n, k))


def test_es_whitened_bitwise():
    fx = Fixture(es_model(), 3)
    check_bitwise("es", fx.model, fx.points(4))


def test_radon_whitened_bitwise():
    fx = Fixture(radon_model(), 5)
    check_bitwise("radon", fx.model, fx.points(4))


def test_lowrank_whitened_bitwise():
    inner = radon_model()
    rng = np.random.RandomState(9)
    n = inner.n_unconstrained
    q_hat = 0.2 * rng.randn(n)
    perm = rng.permutation(n)
    L = scipy.sparse.csc_matrix(random_factor(n, rng))
    whitened = inner.whiten(q_hat, perm, L, lowrank=lowrank_for(n, rng))
    check_bitwise("radon lowrank", whitened,
                  [0.4 * rng.randn(n) for _ in range(4)])


def test_laplace_whitened_bitwise():
    inner = radon_model()
    whitened, _ = stanli.laplace_whiten(inner, latent=["alpha_raw"], seed=1)
    rng = np.random.RandomState(2)
    check_bitwise("radon laplace", whitened,
                  [0.5 * rng.randn(whitened.n_unconstrained)
                   for _ in range(3)])


def test_plain_model_has_no_whiten_block():
    inner = es_model()
    manifest = json.loads(inner.bridgestan_data())["__stanli"]
    assert "whiten" not in manifest
    assert manifest["name"] == "stanli_model"
    lib = bind()
    m = construct(lib, inner.bridgestan_data())
    try:
        z = np.linspace(-0.5, 0.5, inner.n_unconstrained)
        lp, g = inner.log_prob_grad(z)
        got_lp, got_g = bs_grad(lib, m, z)
        assert got_lp == lp and np.array_equal(got_g, g)
    finally:
        lib.bs_model_destruct(m)


def test_whiten_manifest_errors_are_loud():
    fx = Fixture(es_model(), 3)
    payload = json.loads(fx.model.bridgestan_data())
    lib = bind()

    def refused(mutate, fragment):
        p = json.loads(json.dumps(payload))
        mutate(p["__stanli"]["whiten"])
        err = ctypes.c_char_p()
        m = lib.bs_model_construct(json.dumps(p).encode(), 1,
                                   ctypes.byref(err))
        assert not m, fragment
        assert err.value and fragment in err.value.decode(), err.value

    refused(lambda w: w.pop("perm"), "perm")
    refused(lambda w: w.update(q_hat="@@@@"), "base64")
    refused(lambda w: w.update(q_hat=w["q_hat"][:-12]), "parameter")
    n = fx.n
    repeated = np.zeros(n, dtype="<i8")
    refused(lambda w: w.update(
        perm=base64.b64encode(repeated.tobytes()).decode()), "permutation")
    refused(lambda w: w.update(perm=w["row_idx"]), "disagree")


def hmc_draws(lib, m, n, draws, rng, eps, steps):
    z = np.zeros(n)
    lp, g = bs_grad(lib, m, z)
    out, accepted = [], 0
    for it in range(draws + 300):
        p = rng.randn(n)
        h0 = -lp + 0.5 * p @ p
        zn, pn, gn = z.copy(), p.copy(), g.copy()
        e = eps * rng.uniform(0.8, 1.2)
        for _ in range(steps):
            pn = pn + 0.5 * e * gn
            zn = zn + e * pn
            lpn, gn = bs_grad(lib, m, zn)
            pn = pn + 0.5 * e * gn
        h1 = -lpn + 0.5 * pn @ pn
        if np.isfinite(h1) and np.log(rng.rand()) < h0 - h1:
            z, lp, g = zn, lpn, gn
            accepted += it >= 300
        if it >= 300:
            out.append(bs_constrain(lib, m, z, False))
    return np.array(out), accepted / draws


def test_sampling_through_bridgestan_matches_in_process():
    try:
        import bridgestan  # noqa: F401
    except ImportError:
        print("skip: bridgestan not installed, sampling not compared")
        return
    inner = radon_model()
    whitened, _ = stanli.laplace_whiten(inner, latent=["alpha_raw"], seed=1)
    model = whitened.bridgestan_model(name="radon_whitened")
    assert model.param_unc_num() == whitened.n_unconstrained
    lib = bind()
    m = construct(lib, whitened.bridgestan_data())
    try:
        draws, accept = hmc_draws(lib, m, whitened.n_unconstrained, 3000,
                                  np.random.RandomState(4), 0.35, 8)
    finally:
        lib.bs_model_destruct(m)
    assert accept > 0.5, accept
    fit = whitened.sample(chains=4, seed=7, warmup=200, samples=1000,
                          metric="unit_e", adapt_metric=False, refresh=0)
    names = whitened.constrained_names
    all_draws = fit.draws()
    ref = all_draws.reshape(-1, all_draws.shape[-1])[:, :len(names)]
    assert list(fit.names[:len(names)]) == list(names)
    sd = ref.std(axis=0)
    ess = max(40.0, draws.shape[0] / 8)
    gap = np.abs(draws.mean(axis=0) - ref.mean(axis=0)) / (sd / np.sqrt(ess))
    print(f"max standardized gap {gap.max():.2f}, acceptance {accept:.2f}")
    assert gap.max() < 6.0, dict(zip(names, gap))


def main():
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"ok   {name}")
            except Exception as exc:
                failures += 1
                print(f"FAIL {name}: {type(exc).__name__}: {exc}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
