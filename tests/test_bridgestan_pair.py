"""The BridgeStan lib pair, exercised the way a sampler loads it.

BridgeStan's clients dlopen a per-model shared library and call bs_* on
it. stanli has one universal library, so a "model library" is a pair: a
copy of the runtime named for the model, and a sidecar manifest beside it
holding the compiled MIR. bs_model_construct finds the sidecar by asking
where its own code is loaded from.

That last step is the whole trick and it only happens under a real
dlopen, which is why this test is here rather than in the C++ suite: a
linked test would resolve the anchor to the test binary and never touch a
manifest. Two models are loaded at once, because one pair reading its own
manifest proves nothing about two.

Run against an installed wheel (CI does) or a local build:

    PYTHONPATH=python python3 tests/test_bridgestan_pair.py
"""
import ctypes
import pathlib
import sys

import stanli

NORMAL = "parameters { real x; } model { x ~ normal(0, 1); }"
# A second model that differs in every way the ABI reports: name, number
# of parameters, and the value at a shared point.
TWO = """
parameters { real a; real<lower=0> b; }
model { a ~ normal(0, 1); b ~ exponential(1); }
generated quantities { real s = a + b; }
"""

failures = []


def check(what, ok):
    if not ok:
        failures.append(what)
        print(f"FAIL {what}")


def bind(lib):
    """The slice of the BridgeStan ABI this test drives."""
    lib.bs_model_construct.restype = ctypes.c_void_p
    lib.bs_model_construct.argtypes = [ctypes.c_char_p, ctypes.c_uint,
                                       ctypes.POINTER(ctypes.c_char_p)]
    lib.bs_model_destruct.argtypes = [ctypes.c_void_p]
    lib.bs_name.restype = ctypes.c_char_p
    lib.bs_name.argtypes = [ctypes.c_void_p]
    lib.bs_param_unc_num.restype = ctypes.c_int
    lib.bs_param_unc_num.argtypes = [ctypes.c_void_p]
    lib.bs_param_num.restype = ctypes.c_int
    lib.bs_param_num.argtypes = [ctypes.c_void_p, ctypes.c_bool, ctypes.c_bool]
    lib.bs_log_density.restype = ctypes.c_int
    lib.bs_log_density.argtypes = [ctypes.c_void_p, ctypes.c_bool,
                                   ctypes.c_bool,
                                   ctypes.POINTER(ctypes.c_double),
                                   ctypes.POINTER(ctypes.c_double),
                                   ctypes.POINTER(ctypes.c_char_p)]
    lib.bs_free_error_msg.argtypes = [ctypes.c_char_p]
    return lib


def construct(lib, data=None):
    err = ctypes.c_char_p()
    m = lib.bs_model_construct(None if data is None else data.encode(), 1234,
                               ctypes.byref(err))
    if not m:
        msg = err.value.decode() if err.value else "(no message)"
        raise RuntimeError(f"bs_model_construct failed: {msg}")
    return m


def log_density(lib, m, q):
    arr = (ctypes.c_double * len(q))(*q)
    val = ctypes.c_double()
    err = ctypes.c_char_p()
    rc = lib.bs_log_density(m, True, True, arr, ctypes.byref(val),
                            ctypes.byref(err))
    if rc != 0:
        raise RuntimeError(err.value.decode() if err.value else "(no message)")
    return val.value


def test_pair_is_written_and_is_content_addressed(tmp):
    a = stanli.Model(stan_code=NORMAL).bridgestan_lib(tmp)
    check("the library exists", a.exists())
    manifest = a.with_suffix("").with_suffix(".stanli.json") \
        if a.suffix else None
    # The helper names the sidecar; ask it rather than guessing the rule.
    sidecar = stanli.bridgestan_manifest_path(a)
    check("the manifest exists", sidecar.exists())
    del manifest

    # Same model, same runtime: the same pair, not a second copy.
    again = stanli.Model(stan_code=NORMAL).bridgestan_lib(tmp)
    check("the same model reuses its pair", again == a)

    # A different model is a different pair.
    b = stanli.Model(stan_code=TWO).bridgestan_lib(tmp)
    check("a different model gets a different pair", b != a)
    return a, b


def test_two_models_load_at_once(a, b):
    """Each clone must read ITS OWN manifest, not the other's."""
    la, lb = bind(ctypes.CDLL(str(a))), bind(ctypes.CDLL(str(b)))
    ma, mb = construct(la), construct(lb)

    check("model A has 1 unconstrained parameter", la.bs_param_unc_num(ma) == 1)
    check("model B has 2 unconstrained parameters",
          lb.bs_param_unc_num(mb) == 2)
    # B alone has a generated quantity, so its constrained count grows with
    # the flag and A's does not.
    check("A has no generated quantities",
          la.bs_param_num(ma, True, True) == la.bs_param_num(ma, True, False))
    check("B has one generated quantity",
          lb.bs_param_num(mb, True, True) ==
          lb.bs_param_num(mb, True, False) + 1)

    # The densities are the models' own, not one model answering twice.
    lp_a = log_density(la, ma, [0.5])
    lp_b = log_density(lb, mb, [0.5, 0.25])
    want_a = stanli.Model(stan_code=NORMAL).log_prob_grad([0.5])[0]
    want_b = stanli.Model(stan_code=TWO).log_prob_grad([0.5, 0.25])[0]
    check(f"A's density is A's ({lp_a} vs {want_a})", lp_a == want_a)
    check(f"B's density is B's ({lp_b} vs {want_b})", lp_b == want_b)

    la.bs_model_destruct(ma)
    lb.bs_model_destruct(mb)


def test_a_stale_manifest_is_refused(tmp):
    """A manifest from another build must not be read as if it were ours."""
    import json
    lib = stanli.Model(stan_code=NORMAL).bridgestan_lib(tmp)
    sidecar = stanli.bridgestan_manifest_path(lib)
    good = json.loads(sidecar.read_text())
    sidecar.write_text(json.dumps({**good, "build_id": "some-other-build"}))
    try:
        construct(bind(ctypes.CDLL(str(lib))))
    except RuntimeError as e:
        check(f"the message names the mismatch: {e}", "build" in str(e))
        sidecar.write_text(json.dumps(good))
        return
    sidecar.write_text(json.dumps(good))
    check("a build id mismatch is refused", False)


def main():
    import tempfile
    tmp = pathlib.Path(tempfile.mkdtemp())
    a, b = test_pair_is_written_and_is_content_addressed(tmp)
    test_two_models_load_at_once(a, b)
    test_a_stale_manifest_is_refused(pathlib.Path(tempfile.mkdtemp()))
    if failures:
        print(f"\n{len(failures)} FAILED")
        return 1
    print("\nall passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
