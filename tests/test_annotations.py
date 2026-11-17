#!/usr/bin/env python3
"""Declaration annotations reach the runtime and drive stanli.laplace_whiten.

Needs numpy and scipy. Run against an installed wheel or a local build:

    PYTHONPATH=python python3 tests/test_annotations.py
"""
import contextlib
import pathlib
import subprocess
import sys
import tempfile
import warnings
from unittest import mock

try:
    import numpy as np
    import scipy.sparse
except ImportError as exc:
    print(f"SKIP {pathlib.Path(__file__).name}: needs numpy and scipy ({exc})")
    sys.exit(0)

import stanli

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_laplace_whiten import CENTRED, J  # noqa: E402
from test_whiten import radon_data  # noqa: E402

ANNOTATED = CENTRED.replace("  vector[J] alpha;", "  @latent vector[J] alpha;")
TWO_LATENT = """
data { int<lower=1> J; vector[J] y; }
parameters {
  @latent vector[J] a;
  @latent @foo real mu;
  real<lower=0> sigma;
}
model { a ~ normal(mu, sigma); y ~ normal(a, 1); }
"""
TRANSFORMED = """
data { int<lower=1> J; vector[J] y; }
parameters { vector[J] a_raw; real<lower=0> sigma; }
transformed parameters { @latent vector[J] a = sigma * a_raw; }
model { a_raw ~ normal(0, 1); sigma ~ normal(0, 1); y ~ normal(a, 1); }
"""
SMALL = {"J": 3, "y": [0.4, -1.2, 2.5]}


def quiet(fn, *args, **kwargs):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        return fn(*args, **kwargs)


def compiler_paths():
    paths = []
    if stanli._lib.stanli_has_embedded_stanc():
        paths.append(contextlib.nullcontext)
    suffix = ".exe" if sys.platform == "win32" else ""
    if (stanli._BIN / ("stanli-compile" + suffix)).is_file():
        paths.append(lambda: mock.patch.object(
            stanli._lib, "stanli_has_embedded_stanc", return_value=0))
    return paths


def sexp_mir(code):
    stanc = HERE.parent / "deps" / "stanc3" / "stanc"
    if not stanc.is_file():
        return None
    with tempfile.TemporaryDirectory() as tmp:
        source = pathlib.Path(tmp) / "m.stan"
        source.write_text(code, encoding="utf-8")
        done = subprocess.run(
            [str(stanc), "--O1", "--debug-optimized-mir", str(source)],
            capture_output=True, text=True, check=False)
    return done.stdout if done.returncode == 0 else None


def refused(match, *args, **kwargs):
    try:
        stanli.laplace_whiten(*args, **kwargs)
    except (ValueError, TypeError) as e:
        assert match in str(e), str(e)
    else:
        raise AssertionError("expected laplace_whiten to refuse: " + match)


def test_annotations_reach_the_model_on_every_compile_path():
    paths = compiler_paths()
    assert paths, "no compile path available"
    for path in paths:
        with path():
            m = quiet(stanli.Model, stan_code=TWO_LATENT, data=SMALL)
            assert m.parameter_annotations() == {
                "a": ["latent"], "mu": ["latent", "foo"]}, \
                m.parameter_annotations()
            plain = stanli.Model(stan_code=TWO_LATENT.replace(
                "@latent ", "").replace("@foo ", ""), data=SMALL)
            assert plain.parameter_annotations() == {}


def test_sexp_and_portable_paths_agree():
    sexp = sexp_mir(TWO_LATENT)
    if sexp is None:
        print("  no stanc binary under deps/stanc3; sexp path skipped")
        return
    portable = quiet(stanli.stan_to_mir, TWO_LATENT)
    assert portable.startswith("STANLI5:")
    from_sexp = quiet(stanli.Model, mir=sexp, data=SMALL)
    from_portable = quiet(stanli.Model, mir=portable, data=SMALL)
    assert (from_sexp.parameter_annotations()
            == from_portable.parameter_annotations()
            == {"a": ["latent"], "mu": ["latent", "foo"]})


def test_transformed_parameter_annotation_is_not_latent():
    m = stanli.Model(stan_code=TRANSFORMED, data=SMALL)
    assert m.parameter_annotations() == {}
    refused("no parameter is annotated @latent", m)


def test_no_annotations_is_a_clear_error():
    m = stanli.Model(stan_code=CENTRED, data=radon_data(J=J, N=120, seed=5))
    assert m.parameter_annotations() == {}
    refused("pass latent=", m)
    refused("annotate", m)


def test_latent_argument_wins_over_annotations():
    m = stanli.Model(stan_code=ANNOTATED, data=radon_data(J=J, N=120, seed=5))
    _, report = stanli.laplace_whiten(m, latent=["beta"], seed=1)
    assert list(report.latent_index) == [2 + J, 3 + J]


def test_annotated_model_whitens_like_explicit_latent():
    data = radon_data(J=J, N=120, seed=5)
    annotated = stanli.Model(stan_code=ANNOTATED, data=data)
    explicit = stanli.Model(stan_code=CENTRED, data=data)
    assert annotated.parameter_annotations() == {"alpha": ["latent"]}
    got_model, got = stanli.laplace_whiten(annotated, seed=1)
    want_model, want = stanli.laplace_whiten(explicit, latent=["alpha"],
                                             seed=1)
    assert list(got.latent_index) == list(want.latent_index)
    assert got.n_theta == want.n_theta
    assert np.allclose(got.theta_hat, want.theta_hat, rtol=1e-8, atol=1e-8)
    assert np.allclose(got.q_hat, want.q_hat, rtol=1e-8, atol=1e-8)
    assert np.array_equal(got.perm, want.perm)
    assert (got.L - want.L).nnz == 0 or abs(got.L - want.L).max() <= 1e-8
    assert got.gradients == want.gradients
    assert (got.nnz_L, got.rank, got.n_clipped, got.n_colors, got.ordering) == (
        want.nnz_L, want.rank, want.n_clipped, want.n_colors, want.ordering)
    assert got_model.parameter_annotations() == {"alpha": ["latent"]}
    q = np.linspace(-0.3, 0.4, got_model.n_unconstrained)
    a, b = got_model.log_prob_grad(q), want_model.log_prob_grad(q)
    assert abs(a[0] - b[0]) <= 1e-8 * max(1.0, abs(b[0]))
    assert np.allclose(a[1], b[1], rtol=1e-8, atol=1e-8)


def test_annotated_radon_sampling_matches_default_sampler():
    data = radon_data(J=20, N=240)
    inner = stanli.Model(stan_code=ANNOTATED, data=data)
    whitened, report = stanli.laplace_whiten(inner, seed=1)
    assert report.n_theta == inner.n_unconstrained - 20
    reference = inner.sample(chains=4, seed=21, warmup=1000, samples=1000,
                             refresh=0)
    fit = whitened.sample(chains=4, seed=22, warmup=200, samples=1000,
                          metric="unit_e", adapt_metric=False, refresh=0)
    assert fit.names == reference.names
    ref_summary, new_summary = reference.summary(), fit.summary()
    worst = 0.0
    for name in fit.names[:len(inner.constrained_names)]:
        a, b = ref_summary[name], new_summary[name]
        mcse = np.hypot(a["mcse_mean"], b["mcse_mean"])
        worst = max(worst, abs(a["mean"] - b["mean"]) / mcse)
    print(f"  worst posterior-mean gap: {worst:.2f} MCSE")
    assert worst <= 4.0, worst


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
