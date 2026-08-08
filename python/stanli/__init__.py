"""stanli: the Stan Language Interpreter.

Compiles a .stan model with stanc3 (linked into the bundled shared library,
or a bundled stanc binary as a subprocess where it is not), lowers it to an
op graph in-process, and samples with NUTS. No C++ toolchain, no model
compilation on this machine.
"""
import ctypes
import json
import pathlib
import subprocess
import sys

import numpy as np

__all__ = ["Model", "__version__"]
# The one place the version lives. setup.py and the release workflow both
# read it from here.
__version__ = "0.4.1"

_BIN = pathlib.Path(__file__).parent / "_bin"


def _load_lib():
    names = {"darwin": "libstanli.dylib", "linux": "libstanli.so"}
    lib = ctypes.CDLL(str(_BIN / names.get(sys.platform, "stanli.dll")))
    lib.stanli_model_new.restype = ctypes.c_void_p
    lib.stanli_model_new.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                     ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_model_free.argtypes = [ctypes.c_void_p]
    lib.stanli_n_unconstrained.restype = ctypes.c_int64
    lib.stanli_n_unconstrained.argtypes = [ctypes.c_void_p]
    lib.stanli_grad.restype = ctypes.c_int
    lib.stanli_grad.argtypes = [ctypes.c_void_p,
                                ctypes.POINTER(ctypes.c_double),
                                ctypes.POINTER(ctypes.c_double),
                                ctypes.POINTER(ctypes.c_double)]
    lib.stanli_sample.restype = ctypes.c_int
    lib.stanli_sample.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                  ctypes.c_int, ctypes.c_int, ctypes.c_double,
                                  ctypes.POINTER(ctypes.c_double),
                                  ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_n_constrained.restype = ctypes.c_int64
    lib.stanli_n_constrained.argtypes = [ctypes.c_void_p]
    lib.stanli_constrained_name.restype = ctypes.c_char_p
    lib.stanli_constrained_name.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    lib.stanli_constrain.restype = ctypes.c_int
    lib.stanli_constrain.argtypes = [ctypes.c_void_p,
                                     ctypes.POINTER(ctypes.c_double),
                                     ctypes.POINTER(ctypes.c_double)]
    lib.stanli_has_embedded_stanc.restype = ctypes.c_int
    lib.stanli_exact_lp.restype = ctypes.c_int
    lib.stanli_model_new_from_stan.restype = ctypes.c_void_p
    lib.stanli_model_new_from_stan.argtypes = [ctypes.c_char_p,
                                               ctypes.c_char_p,
                                               ctypes.c_char_p,
                                               ctypes.c_size_t]
    lib.stanli_wa_n_columns.restype = ctypes.c_int64
    lib.stanli_wa_n_columns.argtypes = [ctypes.c_void_p]
    lib.stanli_wa_column_name.restype = ctypes.c_char_p
    lib.stanli_wa_column_name.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    lib.stanli_wa_seed.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.stanli_wa_row.restype = ctypes.c_int
    lib.stanli_wa_row.argtypes = [ctypes.c_void_p,
                                  ctypes.POINTER(ctypes.c_double),
                                  ctypes.POINTER(ctypes.c_double)]
    return lib


_lib = _load_lib()


def _stanc_mir(model_path: pathlib.Path) -> str:
    stanc = _BIN / ("stanc.exe" if sys.platform == "win32" else "stanc")
    r = subprocess.run([str(stanc), "--debug-transformed-mir",
                        str(model_path)],
                       capture_output=True, text=True)
    if r.returncode != 0 or not r.stdout:
        raise RuntimeError(f"stanc failed:\n{r.stderr}")
    return r.stdout


def _data_to_json(data) -> str:
    """Accept what callers naturally have: a dict, a path, or JSON text.

    numpy arrays are converted, since data almost always arrives as one.
    """
    if data is None:
        return "{}"
    if isinstance(data, pathlib.Path):
        return data.read_text()
    if isinstance(data, str):
        stripped = data.lstrip()
        if stripped.startswith("{"):
            return data
        return pathlib.Path(data).read_text()

    def encode(value):
        if hasattr(value, "tolist"):
            return value.tolist()
        raise TypeError(f"cannot serialise {type(value).__name__} as Stan data")

    return json.dumps(data, default=encode)


def exact_lp() -> bool:
    """True if lp__ reproduces CmdStan's exactly.

    The wheel is built this way and always has been. A STANLI_LITE_LP
    build -- which is what ships to the browser -- drops stan-math's
    propto instantiations to halve the library, leaving every gradient
    bitwise identical and lp__ a per-model constant higher. A pinned seed
    still gives a different chain there: lp is added to the kinetic
    energy, so shifting it changes the rounding, and NUTS amplifies that
    into a different (equally valid) trajectory.
    """
    return bool(_lib.stanli_exact_lp())


class Model:
    """A compiled (model, data) pair."""

    def __init__(self, stan_file=None, data=None, stan_code=None):
        if stan_code is None:
            if stan_file is None:
                raise ValueError("provide stan_file or stan_code")
            stan_code = pathlib.Path(stan_file).read_text()
        data_json = _data_to_json(data)

        err = ctypes.create_string_buffer(8192)
        if _lib.stanli_has_embedded_stanc():
            # Fully in-process: embedded stanc3 compiles the model.
            self._m = _lib.stanli_model_new_from_stan(
                stan_code.encode(), data_json.encode(), err, len(err))
        else:
            # Fallback: bundled stanc binary as a subprocess.
            import tempfile
            tmp = pathlib.Path(tempfile.mkdtemp()) / "model.stan"
            tmp.write_text(stan_code)
            mir = _stanc_mir(tmp)
            self._m = _lib.stanli_model_new(mir.encode(), data_json.encode(),
                                            err, len(err))
        if not self._m:
            raise RuntimeError(err.value.decode())
        self.n_unconstrained = _lib.stanli_n_unconstrained(self._m)
        n_con = _lib.stanli_n_constrained(self._m)
        self.constrained_names = [
            _lib.stanli_constrained_name(self._m, i).decode()
            for i in range(n_con)
        ]

    def __del__(self):
        if getattr(self, "_m", None):
            _lib.stanli_model_free(self._m)
            self._m = None

    def log_prob_grad(self, q):
        """log density (jacobian included) and gradient at unconstrained q."""
        q = np.ascontiguousarray(q, dtype=np.float64)
        if q.size != self.n_unconstrained:
            raise ValueError(f"q has {q.size} elements, model has "
                             f"{self.n_unconstrained} unconstrained parameters")
        lp = ctypes.c_double()
        grad = np.empty(self.n_unconstrained)
        rc = _lib.stanli_grad(
            self._m,
            q.ctypes.data_as(ctypes.POINTER(ctypes.c_double)),
            ctypes.byref(lp),
            grad.ctypes.data_as(ctypes.POINTER(ctypes.c_double)))
        if rc != 0:
            # The gradient buffer is uninitialized on failure; never let a
            # caller see it.
            raise RuntimeError("log density evaluation failed at this point "
                               "(domain error in a distribution or function)")
        return lp.value, grad

    def sample(self, *, seed=1, warmup=1000, samples=1000, delta=0.8):
        """NUTS draws as {name: array} of CSV columns.

        Models with a generate_quantities section return every column
        CmdStan's CSV would carry: constrained parameters, transformed
        parameters, and generated quantities, with RNG draws streamed
        from `seed`. Models without one return the constrained
        parameters.
        """
        n = self.n_unconstrained
        draws = np.empty((samples, n))
        err = ctypes.create_string_buffer(4096)
        rc = _lib.stanli_sample(
            self._m, seed, warmup, samples, delta,
            draws.ctypes.data_as(ctypes.POINTER(ctypes.c_double)),
            err, len(err))
        if rc != 0:
            raise RuntimeError(err.value.decode())

        n_wa = _lib.stanli_wa_n_columns(self._m)
        if n_wa > 0:
            names = [_lib.stanli_wa_column_name(self._m, i).decode()
                     for i in range(n_wa)]
            _lib.stanli_wa_seed(self._m, seed)
            out = np.empty((samples, n_wa))
            row = np.empty(n_wa)
            for s in range(samples):
                if _lib.stanli_wa_row(
                        self._m,
                        draws[s].ctypes.data_as(
                            ctypes.POINTER(ctypes.c_double)),
                        row.ctypes.data_as(
                            ctypes.POINTER(ctypes.c_double))) != 0:
                    raise RuntimeError(
                        f"write_array failed on draw {s}")
                out[s] = row
            return {name: out[:, i] for i, name in enumerate(names)}

        n_con = len(self.constrained_names)
        con = np.empty((samples, n_con))
        row = np.empty(n_con)
        for s in range(samples):
            _lib.stanli_constrain(
                self._m,
                draws[s].ctypes.data_as(ctypes.POINTER(ctypes.c_double)),
                row.ctypes.data_as(ctypes.POINTER(ctypes.c_double)))
            con[s] = row
        return {name: con[:, i]
                for i, name in enumerate(self.constrained_names)}
