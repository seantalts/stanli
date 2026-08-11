"""The embedded-manifest transport, exercised the way a sampler uses it.

Where the pair test dlopens a per-model copy of the runtime and lets
bs_model_construct find its sidecar, this test dlopens the ONE runtime
library directly and hands the model in through the data argument under
"__stanli". Two different models through the same library handle is the
property that matters: with the pair transport that aliasing is exactly
what breaks, and with this one it is the point.

Run against an installed wheel (CI does) or a local build:

    PYTHONPATH=python python3 tests/test_bridgestan_embed.py
"""
import ctypes
import json
import pathlib
import sys

import stanli

NORMAL = "parameters { real x; } model { x ~ normal(0, 1); }"
DATED = """
data { int<lower=0> N; vector[N] y; }
parameters { real theta; }
model { theta ~ normal(0, 1); y ~ normal(theta, 1); }
"""

failures = []


def check(what, ok):
    if not ok:
        failures.append(what)
        print(f"FAIL {what}")


def bind(lib):
    lib.bs_model_construct.restype = ctypes.c_void_p
    lib.bs_model_construct.argtypes = [ctypes.c_char_p, ctypes.c_uint,
                                       ctypes.POINTER(ctypes.c_char_p)]
    lib.bs_model_destruct.argtypes = [ctypes.c_void_p]
    lib.bs_name.restype = ctypes.c_char_p
    lib.bs_name.argtypes = [ctypes.c_void_p]
    lib.bs_param_unc_num.restype = ctypes.c_int
    lib.bs_param_unc_num.argtypes = [ctypes.c_void_p]
    lib.bs_log_density.restype = ctypes.c_int
    lib.bs_log_density.argtypes = [ctypes.c_void_p, ctypes.c_bool,
                                   ctypes.c_bool,
                                   ctypes.POINTER(ctypes.c_double),
                                   ctypes.POINTER(ctypes.c_double),
                                   ctypes.POINTER(ctypes.c_char_p)]
    lib.bs_free_error_msg.argtypes = [ctypes.c_char_p]
    return lib


def embed(stan_code, name, data=None):
    payload = dict(data or {})
    payload["__stanli"] = {"build_id": stanli.build_id(),
                          "mir": stanli.stan_to_mir(stan_code),
                          "name": name}
    return json.dumps(payload)


def construct(lib, data):
    err = ctypes.c_char_p()
    m = lib.bs_model_construct(data.encode(), 1234, ctypes.byref(err))
    return m, (err.value.decode() if err.value else None)


def log_density(lib, m, q):
    arr = (ctypes.c_double * len(q))(*q)
    val = ctypes.c_double()
    err = ctypes.c_char_p()
    rc = lib.bs_log_density(m, True, True, arr, ctypes.byref(val),
                            ctypes.byref(err))
    if rc != 0:
        raise RuntimeError(err.value.decode() if err.value else "(no message)")
    return val.value


def test_two_models_through_one_library():
    lib = bind(ctypes.CDLL(str(stanli._runtime_lib_path())))
    ma, err = construct(lib, embed(NORMAL, "n"))
    check(f"the data-free model constructs ({err})", ma)
    mb, err = construct(lib, embed(DATED, "d", {"N": 3, "y": [0.1, 0.2, 0.3]}))
    check(f"the model with data constructs ({err})", mb)
    if not (ma and mb):
        return
    check("each model keeps its own name",
          lib.bs_name(ma) == b"n" and lib.bs_name(mb) == b"d")
    check("each model keeps its own parameters",
          lib.bs_param_unc_num(ma) == 1 and lib.bs_param_unc_num(mb) == 1)
    # Two evaluations at the same point must give each model's OWN density.
    la, lb = log_density(lib, ma, [0.5]), log_density(lib, mb, [0.5])
    check("the two models disagree at a shared point", la != lb)
    # And the values themselves, against stanli's front door.
    want_a = stanli.Model(stan_code=NORMAL).log_prob_grad([0.5])[0]
    want_b = stanli.Model(stan_code=DATED,
                          data={"N": 3, "y": [0.1, 0.2, 0.3]
                                }).log_prob_grad([0.5])[0]
    check("data-free density matches stanli.Model", la == want_a)
    check("with-data density matches stanli.Model", lb == want_b)
    lib.bs_model_destruct(ma)
    lib.bs_model_destruct(mb)


def test_wrong_build_is_loud():
    lib = bind(ctypes.CDLL(str(stanli._runtime_lib_path())))
    payload = json.loads(embed(NORMAL, "n"))
    payload["__stanli"]["build_id"] = "abi1-deadbeef-Linux-x86_64"
    m, err = construct(lib, json.dumps(payload))
    check("a stale embedded manifest is refused", not m)
    check("the refusal names the build mismatch",
          err is not None and "abi1-deadbeef" in err)
    if m:
        lib.bs_model_destruct(m)


def test_bridgestan_model_sugar():
    try:
        import bridgestan  # noqa: F401
    except ImportError:
        print("skip: bridgestan not installed, sugar not exercised")
        return
    model = stanli.bridgestan_model(stan_code=DATED, name="d",
                                    data={"N": 3, "y": [0.1, 0.2, 0.3]})
    check("bridgestan_model names the model", model.name() == "d")
    want = stanli.Model(stan_code=DATED,
                        data={"N": 3, "y": [0.1, 0.2, 0.3]
                              }).log_prob_grad([0.5])[0]
    import numpy as np
    check("bridgestan_model density matches stanli.Model",
          model.log_density(np.array([0.5]), propto=True,
                            jacobian=True) == want)


def main():
    test_two_models_through_one_library()
    test_wrong_build_is_loud()
    test_bridgestan_model_sugar()
    if failures:
        print(f"{len(failures)} failures")
        return 1
    print("test_bridgestan_embed OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
