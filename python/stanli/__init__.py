"""stanli: the Stan Language Interpreter.

Compiles a .stan model with stanc3 (linked into the bundled shared library,
or a bundled compiler executable as a subprocess where it is not), lowers it
to an op graph in-process, and samples with NUTS. No C++ toolchain, no model
compilation on this machine.
"""
import ctypes
from collections.abc import Mapping
import json
import math
import operator
import os
import pathlib
import subprocess
import sys

import numpy as np

__all__ = ["Model", "Function", "Fit", "Summary", "OptimizeResult",
           "exact_lp", "thread_safe", "stan_to_mir", "build_id",
           "bridgestan_model",
           "SAMPLER_COLUMNS", "__version__"]
# The one place the version lives. setup.py and the release workflow both
# read it from here.
__version__ = "0.10.0"

_BIN = pathlib.Path(__file__).parent / "_bin"

# Must match STANLI_N_SAMPLER_COLS in runtime/include/stanli/capi.h; the
# names themselves come from the ABI (see below), so only the count is
# mirrored here.
_N_SAMPLER_COLS = 7
# Must match the STANLI_STAT_* enum in capi.h.
_SUMMARY_STATS = ("mean", "mcse_mean", "sd", "mcse_sd", "q5", "q50", "q95",
                  "ess_bulk", "ess_tail", "r_hat")
_N_SUMMARY_STATS = len(_SUMMARY_STATS)


class _OptimizeOpts(ctypes.Structure):
    """Mirrors stanli_optimize_opts in runtime/include/stanli/capi.h."""
    _fields_ = [
        ("seed", ctypes.c_uint32),
        ("chain_id", ctypes.c_int),
        ("iter", ctypes.c_int),
        ("jacobian", ctypes.c_int),
        ("init_alpha", ctypes.c_double),
        ("tol_obj", ctypes.c_double),
        ("tol_rel_obj", ctypes.c_double),
        ("tol_grad", ctypes.c_double),
        ("tol_rel_grad", ctypes.c_double),
        ("tol_param", ctypes.c_double),
        ("history_size", ctypes.c_int),
        ("init_radius", ctypes.c_double),
        ("init", ctypes.POINTER(ctypes.c_double)),
    ]


class _SampleOpts(ctypes.Structure):
    """Mirrors stanli_sample_opts in runtime/include/stanli/capi.h.

    Field order and types must match the header exactly. A mismatch is a
    silent misread rather than an error -- the run samples successfully
    from the wrong configuration -- so tests/test_python.py checks the
    defaults this struct comes back with against the documented ones.
    """
    _fields_ = [
        ("seed", ctypes.c_uint32),
        ("chains", ctypes.c_int),
        ("chain_id", ctypes.c_int),
        ("warmup", ctypes.c_int),
        ("samples", ctypes.c_int),
        ("thin", ctypes.c_int),
        ("delta", ctypes.c_double),
        ("max_depth", ctypes.c_int),
        ("save_warmup", ctypes.c_int),
        ("init_radius", ctypes.c_double),
        ("inits", ctypes.POINTER(ctypes.c_double)),
        ("num_threads", ctypes.c_int),
    ]


class _SampleReport(ctypes.Structure):
    """Mirrors stanli_sample_report in runtime/include/stanli/capi.h."""
    _fields_ = [
        ("warmup_seconds", ctypes.c_double),
        ("sampling_seconds", ctypes.c_double),
        ("n_divergent", ctypes.c_int64),
        ("n_max_treedepth", ctypes.c_int64),
    ]


_SampleProgressCallback = ctypes.CFUNCTYPE(
    None, ctypes.c_int32, ctypes.c_int64, ctypes.c_int64, ctypes.c_int32,
    ctypes.c_void_p)


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
    lib.stanli_unconstrain_inits.restype = ctypes.c_int
    lib.stanli_unconstrain_inits.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
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
    # c_void_p, not c_char_p: ctypes converts a c_char_p result to bytes
    # and drops the pointer, which would leak the string stanli handed us
    # ownership of.
    lib.stanli_stan_to_mir.restype = ctypes.c_void_p
    lib.stanli_stan_to_mir.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                       ctypes.c_size_t]
    lib.stanli_string_free.argtypes = [ctypes.c_void_p]
    lib.stanli_build_id.restype = ctypes.c_char_p
    lib.stanli_build_id.argtypes = []
    lib.stanli_wa_n_columns.restype = ctypes.c_int64
    lib.stanli_wa_n_columns.argtypes = [ctypes.c_void_p]
    lib.stanli_wa_column_name.restype = ctypes.c_char_p
    lib.stanli_wa_column_name.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    lib.stanli_wa_seed.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.stanli_wa_seed_chain.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                         ctypes.c_uint32]
    lib.stanli_wa_row.restype = ctypes.c_int
    lib.stanli_wa_row.argtypes = [ctypes.c_void_p,
                                  ctypes.POINTER(ctypes.c_double),
                                  ctypes.POINTER(ctypes.c_double)]
    # Multi-chain sampling, summaries, diagnostics.
    lib.stanli_sample_opts_init.argtypes = [ctypes.POINTER(_SampleOpts)]
    lib.stanli_n_stored_draws.restype = ctypes.c_int64
    lib.stanli_n_stored_draws.argtypes = [ctypes.POINTER(_SampleOpts)]
    lib.stanli_thread_safe.restype = ctypes.c_int
    lib.stanli_sample_multi.restype = ctypes.c_int
    lib.stanli_sample_multi.argtypes = [ctypes.c_void_p,
                                        ctypes.POINTER(_SampleOpts),
                                        ctypes.POINTER(ctypes.c_double),
                                        ctypes.POINTER(ctypes.c_double),
                                        ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_sample_multi_progress.restype = ctypes.c_int
    lib.stanli_sample_multi_progress.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(_SampleOpts), ctypes.c_int,
        ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_double),
        _SampleProgressCallback, ctypes.c_void_p,
        ctypes.POINTER(_SampleReport), ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_pathfinder_inits.restype = ctypes.c_int
    lib.stanli_pathfinder_inits.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int, ctypes.c_int,
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_double,
        ctypes.POINTER(ctypes.c_double), ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_summary_stats.restype = ctypes.c_int
    lib.stanli_summary_stats.argtypes = [ctypes.POINTER(ctypes.c_double),
                                         ctypes.c_int64, ctypes.c_int64,
                                         ctypes.c_int64,
                                         ctypes.POINTER(ctypes.c_double)]
    lib.stanli_optimize_opts_init.argtypes = [ctypes.POINTER(_OptimizeOpts)]
    lib.stanli_optimize.restype = ctypes.c_int
    lib.stanli_optimize.argtypes = [ctypes.c_void_p,
                                    ctypes.POINTER(_OptimizeOpts),
                                    ctypes.POINTER(ctypes.c_double),
                                    ctypes.POINTER(ctypes.c_double),
                                    ctypes.POINTER(ctypes.c_double),
                                    ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_diagnose_text.restype = ctypes.c_int64
    lib.stanli_diagnose_text.argtypes = [ctypes.POINTER(ctypes.c_double),
                                         ctypes.c_int64, ctypes.c_int64,
                                         ctypes.c_int64,
                                         ctypes.POINTER(ctypes.c_char_p),
                                         ctypes.POINTER(ctypes.c_double),
                                         ctypes.c_int, ctypes.c_char_p,
                                         ctypes.c_size_t]
    lib.stanli_sampler_column_name.restype = ctypes.c_char_p
    lib.stanli_sampler_column_name.argtypes = [ctypes.c_int]
    return lib


_lib = _load_lib()

# The column names are the ABI's, not ours; the R bridge reads the same
# accessor.
SAMPLER_COLUMNS = tuple(_lib.stanli_sampler_column_name(i).decode()
                        for i in range(_N_SAMPLER_COLS))


def _dptr(a):
    return a.ctypes.data_as(ctypes.POINTER(ctypes.c_double))


_PATHFINDER_INIT_DEFAULTS = {
    "num_iterations": 1000,
    "num_elbo_draws": 25,
    "history_size": 5,
    "init_radius": 2.0,
}


def _pathfinder_init_options(value):
    """Validate the small, deliberately separate Pathfinder-init surface."""
    if not isinstance(value, Mapping):
        raise TypeError("pathfinder_init must be a mapping of options")
    unknown = set(value) - set(_PATHFINDER_INIT_DEFAULTS)
    if unknown:
        raise ValueError(
            "unknown pathfinder_init option" + ("s" if len(unknown) > 1 else "")
            + ": " + ", ".join(sorted(map(str, unknown))))
    out = dict(_PATHFINDER_INIT_DEFAULTS)
    out.update(value)
    int_max = 2 ** (8 * ctypes.sizeof(ctypes.c_int) - 1) - 1
    for name in ("num_iterations", "num_elbo_draws", "history_size"):
        raw = out[name]
        if isinstance(raw, (bool, np.bool_)):
            raise TypeError(f"pathfinder_init {name} must be a positive integer")
        try:
            parsed = operator.index(raw)
        except TypeError:
            raise TypeError(
                f"pathfinder_init {name} must be a positive integer") from None
        if parsed <= 0:
            raise ValueError(f"pathfinder_init {name} must be positive")
        if parsed > int_max:
            raise OverflowError(f"pathfinder_init {name} does not fit in a C int")
        out[name] = parsed
    radius = out["init_radius"]
    if isinstance(radius, (bool, np.bool_)):
        raise TypeError("pathfinder_init init_radius must be nonnegative")
    try:
        radius = float(radius)
    except (TypeError, ValueError):
        raise TypeError(
            "pathfinder_init init_radius must be nonnegative") from None
    if not math.isfinite(radius) or radius < 0:
        raise ValueError(
            "pathfinder_init init_radius must be finite and nonnegative")
    out["init_radius"] = radius
    return out


def _compiler_command(model_path: pathlib.Path):
    """The packaged compiler argv, preferring stanli's portable producer."""
    suffix = ".exe" if sys.platform == "win32" else ""
    portable = _BIN / ("stanli-compile" + suffix)
    if portable.is_file():
        return [str(portable), str(model_path)]

    # One-cycle rollback path: pristine stanc3 emits the legacy s-expression
    # that the runtime continues to accept. Only absence selects it. A broken
    # portable compiler must fail loudly rather than being hidden by a retry.
    stanc = _BIN / ("stanc" + suffix)
    if stanc.is_file():
        return [str(stanc), "--O1", "--debug-optimized-mir", str(model_path)]
    raise RuntimeError(
        "the bundled Stan compiler is missing "
        f"(expected {portable.name} or {stanc.name} in {_BIN})")


def _stanc_mir(model_path: pathlib.Path) -> str:
    argv = _compiler_command(model_path)
    try:
        r = subprocess.run(argv, capture_output=True, text=True,
                           encoding="utf-8")
    except OSError as exc:
        raise RuntimeError(f"could not run the bundled Stan compiler: {exc}") \
            from exc
    if r.returncode != 0 or not r.stdout:
        raise RuntimeError(f"stanc failed:\n{r.stderr}")
    return r.stdout


def _subprocess_mir(stan_code: str) -> str:
    """Compile source in an isolated directory and remove every side effect."""
    import tempfile
    with tempfile.TemporaryDirectory(prefix="stanli-compile-") as tmpdir:
        source = pathlib.Path(tmpdir) / "model.stan"
        # Binary UTF-8 avoids the locale-dependent encoding and CRLF rewriting
        # of text-mode files on Windows. Stock stanc also writes model.hpp next
        # to this file; TemporaryDirectory removes that rollback-path artifact.
        source.write_bytes(stan_code.encode("utf-8"))
        return _stanc_mir(source)


def _read_utf8_file(path) -> str:
    """Read source bytes as UTF-8 without universal-newline rewriting."""
    return pathlib.Path(path).read_bytes().decode("utf-8")


def stan_to_mir(stan_code: str) -> str:
    """Stan source to optimized-MIR text, without building a model.

    The first half of compiling a model, on its own. Useful when the MIR
    is what you want to keep -- to cache it, ship it, or hand it to
    ``Model(mir=...)`` in another process. Embedded builds and the preferred
    bundled compiler return stanli's versioned portable format. The one-cycle
    stock-compiler fallback returns a legacy stanc3 s-expression, which the
    runtime also accepts.
    """
    if not _lib.stanli_has_embedded_stanc():
        return _subprocess_mir(stan_code)
    err = ctypes.create_string_buffer(8192)
    p = _lib.stanli_stan_to_mir(stan_code.encode(), err, len(err))
    if not p:
        raise RuntimeError(err.value.decode())
    try:
        return ctypes.cast(p, ctypes.c_char_p).value.decode()
    finally:
        _lib.stanli_string_free(p)


def _lib_suffix() -> str:
    return {"darwin": ".dylib", "win32": ".dll"}.get(sys.platform, ".so")


def _runtime_lib_path() -> pathlib.Path:
    return _BIN / ("libstanli" + _lib_suffix() if sys.platform != "win32"
                   else "stanli.dll")


def _resolve_program(stan_file, stan_code, mir, name):
    """(mir, name) from whichever form the caller has, compiling if needed."""
    if mir is None:
        if stan_code is None:
            if stan_file is None:
                raise ValueError("provide stan_file, stan_code, or mir")
            stan_code = _read_utf8_file(stan_file)
        mir = stan_to_mir(stan_code)
    if name is None:
        name = pathlib.Path(stan_file).stem if stan_file else "stanli_model"
    return mir, name


def bridgestan_model(stan_file=None, stan_code=None, mir=None, data=None,
                     name=None, **kw):
    """A ``bridgestan.StanModel`` for this program, with nothing written.

    The model travels inside the data argument: the manifest rides under
    the reserved key ``"__stanli"`` (never a data variable; Stan
    identifiers begin with a letter), and ``bs_model_construct`` reads it
    out before binding the rest as the model's data. Every model shares
    the one runtime library already inside this package, so nothing is
    copied and nothing touches disk. Path-form ``data`` is read and
    inlined to make room for the splice.

    ``bridgestan`` is imported lazily and is not a stanli dependency.
    Extra keyword arguments pass through to ``bridgestan.StanModel``.
    """
    import bridgestan

    mir, name = _resolve_program(stan_file, stan_code, mir, name)
    payload = json.loads(_data_to_json(data))
    if not isinstance(payload, dict):
        raise ValueError("data must be a JSON object")
    payload["__stanli"] = {"build_id": build_id(), "mir": mir, "name": name}
    # bridgestan warns that the library is "already loaded" -- it is, by
    # stanli's own import, and sharing it is this function's design, so
    # the warning is off unless the caller asks for it.
    kw.setdefault("warn", False)
    return bridgestan.StanModel(_runtime_lib_path(), json.dumps(payload),
                                **kw)


def build_id() -> str:
    """Identifies the runtime binary this package loaded.

    Source revision plus the build choices that change what that source
    produces. Key cached artifacts on it and refuse a mismatch.
    """
    return _lib.stanli_build_id().decode()


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


def thread_safe() -> bool:
    """True if this build can run chains in parallel threads.

    stan-math's autodiff stack is a plain static unless STAN_THREADS is
    defined at build time, and the legacy kernels build nested var tapes
    on it. Where this is False, ``parallel_chains`` is clamped to 1 rather
    than honoured -- a slow answer instead of a wrong one.
    """
    return bool(_lib.stanli_thread_safe())


def _fmt(v, prec=4):
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "nan"
    if abs(v) >= 1e5 or (v != 0 and abs(v) < 1e-3):
        return f"{v:.{prec}g}"
    return f"{v:.{prec}f}"


def _print_sample_progress(chain_id, iteration, total, warmup):
    """One CmdStan-shaped progress line, through Python's live stdout."""
    width = max(1, len(str(total)))
    percent = 100 if total == 0 else 100 * iteration // total
    phase = "Warmup" if warmup else "Sampling"
    print(f"Chain [{chain_id}] Iteration: {iteration:{width}d} / {total} "
          f"[{percent:3d}%]  ({phase})", flush=True)


def _print_sample_reports(reports, first_chain, samples, max_depth):
    """Per-chain timing and aggregate post-warmup sampler problems."""
    print()
    for c, report in enumerate(reports):
        label = f"Chain [{first_chain + c}] Elapsed Time: "
        indent = " " * len(label)
        total = report.warmup_seconds + report.sampling_seconds
        print(f"{label}{report.warmup_seconds:.3f} seconds (Warm-up)")
        print(f"{indent}{report.sampling_seconds:.3f} seconds (Sampling)")
        print(f"{indent}{total:.3f} seconds (Total)")
        print()

    n_divergent = sum(r.n_divergent for r in reports)
    n_max_depth = sum(r.n_max_treedepth for r in reports)
    n_transitions = len(reports) * samples
    if n_divergent:
        percent = 100 * n_divergent / n_transitions if n_transitions else 0.0
        print(f"Warning: {n_divergent} of {n_transitions} post-warmup "
              f"transitions ({percent:.1f}%) ended with a divergence.")
    if n_max_depth:
        percent = 100 * n_max_depth / n_transitions if n_transitions else 0.0
        print(f"Warning: {n_max_depth} of {n_transitions} post-warmup "
              f"transitions ({percent:.1f}%) saturated the maximum "
              f"treedepth of {max_depth}.")


class Summary:
    """Per-parameter posterior summary: the columns stansummary prints.

    R-hat is rank-normalized split-R-hat and ESS is the bulk/tail pair,
    both Vehtari et al. 2021, computed by stan's own estimators. A
    constant column (a fixed transformed parameter) reports NaN for both
    rather than a passing number.

    Each statistic is also an attribute holding the whole column, so
    ``summary.r_hat.max()`` works without indexing by name.
    """

    def __init__(self, names, stats):
        self.names = list(names)
        self.stats = stats  # (n_cols, len(_SUMMARY_STATS))
        for i, key in enumerate(_SUMMARY_STATS):
            setattr(self, key, stats[:, i])

    def __len__(self):
        return len(self.names)

    def __getitem__(self, name):
        """The row for one parameter, as a dict."""
        i = self.names.index(name)
        return {k: self.stats[i, j] for j, k in enumerate(_SUMMARY_STATS)}

    def to_pandas(self):
        """A DataFrame, for callers that have pandas. Not a dependency."""
        import pandas as pd
        return pd.DataFrame(self.stats, index=self.names,
                            columns=list(_SUMMARY_STATS))

    def __str__(self):
        w = max([len("name")] + [len(n) for n in self.names])
        head = ["Mean", "MCSE", "StdDev", "5%", "50%", "95%", "ESS_bulk",
                "ESS_tail", "R_hat"]
        out = ["name".ljust(w) + "".join(h.rjust(11) for h in head)]
        for i, n in enumerate(self.names):
            r = self.stats[i]
            cells = [_fmt(r[0]), _fmt(r[1]), _fmt(r[2]), _fmt(r[4]),
                     _fmt(r[5]), _fmt(r[6]), _fmt(r[7], 0), _fmt(r[8], 0),
                     _fmt(r[9], 3)]
            out.append(n.ljust(w) + "".join(c.rjust(11) for c in cells))
        return "\n".join(out)

    __repr__ = __str__


class Fit:
    """Draws from one sampling run, plus the sampler's own diagnostics.

    Indexing by name gives every draw of that column with the chains
    concatenated, which is what an estimate wants and what ``sample()``
    returned before it grew chains::

        fit["mu"].mean()

    ``fit.draws("mu")`` keeps the chain axis -- shape (chains, draws) --
    which is what a trace plot or a per-chain check wants.
    """

    def __init__(self, names, draws, sampler_stats, max_depth, seed,
                 reports=None):
        self.names = list(names)
        self._draws = draws               # (chains, draws, cols)
        self.sampler_stats = sampler_stats  # (chains, draws, 7)
        self.max_depth = max_depth
        self.seed = seed
        self._divergence_counts = None
        self._max_treedepth_counts = None
        if reports is not None:
            self._divergence_counts = np.array(
                [r.n_divergent for r in reports], dtype=np.int64)
            self._max_treedepth_counts = np.array(
                [r.n_max_treedepth for r in reports], dtype=np.int64)

    @property
    def n_chains(self):
        return self._draws.shape[0]

    @property
    def n_draws(self):
        return self._draws.shape[1]

    def draws(self, name=None):
        """(chains, draws) for one column, or (chains, draws, cols) for all."""
        if name is None:
            return self._draws
        return self._draws[:, :, self.names.index(name)]

    def __getitem__(self, name):
        """Every draw of one column, chains concatenated.

        Sampler columns (lp__, divergent__, ...) are reachable by name
        here too, so `fit["lp__"]` works the way a CSV reader would
        expect even though they are not posterior columns.
        """
        if name in SAMPLER_COLUMNS:
            col = SAMPLER_COLUMNS.index(name)
            return self.sampler_stats[:, :, col].reshape(-1)
        return self._draws[:, :, self.names.index(name)].reshape(-1)

    # The dict protocol the pre-chains API returned, so `fit["mu"]` and
    # `for k, v in fit.items()` keep working unchanged.
    def keys(self):
        return list(self.names)

    def items(self):
        return [(n, self[n]) for n in self.names]

    def values(self):
        return [self[n] for n in self.names]

    def __iter__(self):
        return iter(self.names)

    def __len__(self):
        return len(self.names)

    def __contains__(self, name):
        return name in self.names

    @property
    def divergences(self):
        """Divergent transitions per chain."""
        if self._divergence_counts is not None:
            return self._divergence_counts.copy()
        col = SAMPLER_COLUMNS.index("divergent__")
        return self.sampler_stats[:, :, col].sum(axis=1).astype(np.int64)

    @property
    def max_treedepth_hits(self):
        """Transitions that saturated max_depth, per chain."""
        if self._max_treedepth_counts is not None:
            return self._max_treedepth_counts.copy()
        col = SAMPLER_COLUMNS.index("treedepth__")
        return (self.sampler_stats[:, :, col] >= self.max_depth) \
            .sum(axis=1).astype(np.int64)

    @property
    def stepsize(self):
        """Adapted stepsize per chain (constant after warmup)."""
        col = SAMPLER_COLUMNS.index("stepsize__")
        return self.sampler_stats[:, -1, col].copy()

    def ebfmi(self):
        """E-BFMI per chain (Betancourt); below 0.3 is a problem.

        The ratio of the mean square of successive energy differences to
        the marginal energy variance. Low values mean momentum resampling
        is not moving the chain across the energy distribution, so the
        tails go unexplored -- a failure R-hat and ESS are both blind to.
        """
        col = SAMPLER_COLUMNS.index("energy__")
        e = self.sampler_stats[:, :, col]
        num = np.square(np.diff(e, axis=1)).sum(axis=1)
        den = np.square(e - e.mean(axis=1, keepdims=True)).sum(axis=1)
        with np.errstate(invalid="ignore", divide="ignore"):
            return np.where(den > 0, num / den, np.nan)

    def summary(self, params=None):
        """Per-parameter summary. `params` selects columns by name."""
        cols = (list(range(len(self.names))) if params is None
                else [self.names.index(p) for p in params])
        sub = np.ascontiguousarray(self._draws[:, :, cols], dtype=np.float64)
        n_chains, n_draws, n_cols = sub.shape
        out = np.empty((n_cols, _N_SUMMARY_STATS))
        if _lib.stanli_summary_stats(_dptr(sub), n_chains, n_draws, n_cols,
                                     _dptr(out)) != 0:
            raise RuntimeError("summary failed")
        return Summary([self.names[i] for i in cols], out)

    def diagnose(self):
        """Convergence checks as text: what passed, what failed, what to do.

        Covers divergences, treedepth saturation, E-BFMI, R-hat and
        bulk/tail ESS -- the set Betancourt's workflow checks, at the
        thresholds the rank-normalized estimators call for.
        """
        d = np.ascontiguousarray(self._draws, dtype=np.float64)
        st = np.ascontiguousarray(self.sampler_stats, dtype=np.float64)
        n_chains, n_draws, n_cols = d.shape
        arr = (ctypes.c_char_p * n_cols)(*[n.encode() for n in self.names])
        # Two calls: the first sizes the text, the second fills it. The C
        # side returns the length it needs either way.
        need = _lib.stanli_diagnose_text(_dptr(d), n_chains, n_draws, n_cols,
                                         arr, _dptr(st), self.max_depth,
                                         None, 0)
        buf = ctypes.create_string_buffer(max(int(need), 1))
        _lib.stanli_diagnose_text(_dptr(d), n_chains, n_draws, n_cols, arr,
                                  _dptr(st), self.max_depth, buf, len(buf))
        return buf.value.decode()

    def to_arviz(self):
        """An arviz InferenceData with the sampler stats attached.

        arviz is not a dependency; this raises ImportError without it.
        """
        import arviz as az
        posterior = {n: self._draws[:, :, i] for i, n in enumerate(self.names)}
        stats = {n.rstrip("_"): self.sampler_stats[:, :, i]
                 for i, n in enumerate(SAMPLER_COLUMNS)}
        return az.from_dict(posterior=posterior, sample_stats=stats)

    def __repr__(self):
        return (f"<stanli.Fit {self.n_chains} chains x {self.n_draws} draws, "
                f"{len(self.names)} columns>")


class OptimizeResult(dict):
    """The mode, by column name, plus where it is on the sampler's scale.

    A dict so `result["mu"]` reads the way a draw does, with the
    unconstrained point and lp attached for handing to `sample(inits=)`.
    """
    unconstrained = None
    lp = None


class Model:
    """A compiled (model, data) pair."""

    def __init__(self, stan_file=None, data=None, stan_code=None, mir=None):
        data_json = _data_to_json(data)
        err = ctypes.create_string_buffer(8192)

        if mir is not None:
            # Already-compiled MIR, from stan_to_mir or from
            # `stanc --O1 --debug-optimized-mir`. Skips the compiler
            # entirely, which is what a cached or shipped model wants.
            self._m = _lib.stanli_model_new(mir.encode(), data_json.encode(),
                                            err, len(err))
            self._finish_init(err)
            return
        if stan_code is None:
            if stan_file is None:
                raise ValueError("provide stan_file, stan_code, or mir")
            stan_code = _read_utf8_file(stan_file)

        if _lib.stanli_has_embedded_stanc():
            # Fully in-process: embedded stanc3 compiles the model.
            self._m = _lib.stanli_model_new_from_stan(
                stan_code.encode(), data_json.encode(), err, len(err))
        else:
            # Windows and other non-embedded builds use the bundled compiler
            # executable as a short-lived subprocess.
            self._m = _lib.stanli_model_new(_subprocess_mir(stan_code).encode(),
                                            data_json.encode(), err, len(err))
        self._finish_init(err)

    def _finish_init(self, err):
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
        rc = _lib.stanli_grad(self._m, _dptr(q), ctypes.byref(lp), _dptr(grad))
        if rc != 0:
            # The gradient buffer is uninitialized on failure; never let a
            # caller see it.
            raise RuntimeError("log density evaluation failed at this point "
                               "(domain error in a distribution or function)")
        return lp.value, grad

    def unconstrain(self, values):
        """Starting values on the CONSTRAINED scale, as the free vector.

        `values` is a dict of parameter name to value, or a JSON string in
        CmdStan's data format. Every declared parameter must appear, at its
        declared size; a missing, unknown, wrong-length, or out-of-support
        value raises naming the parameter.

        Containers are listed in Stan's own serialization order -- the first
        index fastest, the order a CSV column carries -- and may be nested or
        flat; the declaration owns the shape either way.

        The result is what `sample(inits=...)`, `optimize(init=...)`, and
        `log_prob_grad` all take, so unconstraining is a step you do once per
        starting point rather than a second kind of argument on every call.
        """
        # The same shapes data accepts: a dict, a path, or JSON text.
        document = _data_to_json(values).encode()
        q = np.empty(self.n_unconstrained)
        err = ctypes.create_string_buffer(4096)
        rc = _lib.stanli_unconstrain_inits(self._m, document, _dptr(q), err,
                                           len(err))
        if rc != 0:
            raise ValueError(err.value.decode())
        return q

    def _column_names(self):
        """The CSV columns for a draw, and whether write_array supplies them.

        Every column CmdStan would write when the model has a generated
        quantities section; the constrained parameters otherwise.
        """
        n_wa = _lib.stanli_wa_n_columns(self._m)
        if n_wa > 0:
            return [_lib.stanli_wa_column_name(self._m, i).decode()
                    for i in range(n_wa)], True
        return list(self.constrained_names), False

    def optimize(self, *, seed=1, iter=2000, jacobian=True, init=None,
                 init_radius=2.0):
        """The mode by L-BFGS -- the same optimizer CmdStan's `optimize` runs.

        Returns an OptimizeResult: a {name: float} of every CSV column at
        the mode, matching one draw of what `sample()` returns, with the
        unconstrained point and lp attached. `.unconstrained` is what
        `sample(inits=...)` takes.

        Returns the posterior MODE. CmdStan's `optimize` defaults to
        `jacobian=0`, the penalized maximum likelihood, which stanli
        cannot offer: the Jacobian terms are folded into the graph at
        lowering time. `jacobian=False` raises rather than quietly
        returning the other quantity -- the two differ for any
        constrained parameter, which is most models.
        """
        if not jacobian:
            raise NotImplementedError(
                "stanli folds the Jacobian into the graph at lowering "
                "time, so the penalized maximum likelihood (CmdStan's "
                "jacobian=0) is not available; optimize() returns the "
                "posterior mode")
        opts = _OptimizeOpts()
        _lib.stanli_optimize_opts_init(ctypes.byref(opts))
        opts.seed = seed
        opts.iter = int(iter)
        opts.jacobian = 1
        opts.init_radius = float(init_radius)
        init_arr = None
        if init is not None:
            init_arr = np.ascontiguousarray(init, dtype=np.float64)
            if init_arr.shape != (self.n_unconstrained,):
                raise ValueError(
                    f"init must be ({self.n_unconstrained},) on the "
                    f"unconstrained scale, got {init_arr.shape}")
            opts.init = _dptr(init_arr)

        names, _ = self._column_names()
        q = np.empty(self.n_unconstrained)
        vals = np.empty(len(names))
        lp = ctypes.c_double()
        err = ctypes.create_string_buffer(4096)
        rc = _lib.stanli_optimize(self._m, ctypes.byref(opts), _dptr(q),
                                  _dptr(vals), ctypes.byref(lp), err, len(err))
        del init_arr
        if rc != 0:
            raise RuntimeError(f"optimize failed: {err.value.decode()}")
        out = OptimizeResult({n: vals[i] for i, n in enumerate(names)})
        out.unconstrained = q
        out.lp = lp.value
        return out

    def sample(self, *, chains=4, seed=1, warmup=1000, samples=1000,
               delta=0.8, max_depth=10, thin=1, save_warmup=False,
               inits=None, init_radius=2.0, pathfinder_init=None,
               parallel_chains=None, refresh=100):
        """NUTS draws as a Fit.

        Four chains by default, because R-hat needs more than one and a
        single-chain run cannot be checked for convergence at all. Chain c
        uses CmdStan's stream for (seed, chain id c+1), so a matched seed
        means a matched stream per chain.

        `inits` is on the UNCONSTRAINED scale: one vector shared by every
        chain, or one row per chain. Start from constrained values by
        passing them through `unconstrain()` first -- one scale here means
        one contract for what a start is. `init_radius=0` starts every
        chain at the origin, which is CmdStan's `init=0`.

        `pathfinder_init={}` instead fits one single-path Pathfinder
        approximation and draws one unconstrained start per chain from it.
        The mapping can override `num_iterations` (1000), `num_elbo_draws`
        (25), `history_size` (5), and Pathfinder's own `init_radius` (2).
        It uses this call's seed and is mutually exclusive with `inits`.
        Single-path Pathfinder does not perform PSIS resampling.

        `parallel_chains` defaults to running every chain at once, capped
        at the machine's cores. Threading changes nothing about the
        answer -- the draws are byte-identical to a sequential run,
        because each chain has its own executor and its own RNG stream --
        so it is on by default. A build without thread support clamps it
        to 1 (see ``thread_safe()``).

        `refresh` is the number of transitions between CmdStan-shaped
        progress updates. The default is 100; zero disables all automatic
        progress, timing, and sampler-problem output. Reporting only observes
        the run and does not change draws, sampler statistics, or RNG streams.

        Columns are every column CmdStan's CSV would carry -- constrained
        parameters, transformed parameters, generated quantities, with RNG
        draws streamed per chain -- for models with a generate_quantities
        section, and the constrained parameters otherwise.
        """
        if isinstance(refresh, (bool, np.bool_)):
            raise TypeError("refresh must be a nonnegative integer")
        try:
            refresh = operator.index(refresh)
        except TypeError:
            raise TypeError("refresh must be a nonnegative integer") from None
        if refresh < 0:
            raise ValueError("refresh must be nonnegative")
        int_max = 2 ** (8 * ctypes.sizeof(ctypes.c_int) - 1) - 1
        if refresh > int_max:
            raise OverflowError("refresh does not fit in a C int")

        opts = _SampleOpts()
        _lib.stanli_sample_opts_init(ctypes.byref(opts))
        opts.seed = seed
        opts.chains = int(chains)
        opts.warmup = int(warmup)
        opts.samples = int(samples)
        opts.thin = int(thin)
        opts.delta = float(delta)
        opts.max_depth = int(max_depth)
        opts.save_warmup = 1 if save_warmup else 0
        opts.init_radius = float(init_radius)
        if parallel_chains is None:
            import os
            parallel_chains = min(opts.chains, os.cpu_count() or 1)
        opts.num_threads = int(parallel_chains)

        # Held in a local until sample_multi returns: opts.inits borrows
        # this buffer, so letting it fall out of scope would hand the
        # sampler freed memory.
        init_arr = None
        if inits is not None and pathfinder_init is not None:
            raise ValueError("inits and pathfinder_init are mutually exclusive")
        if pathfinder_init is not None:
            if opts.chains <= 0:
                raise ValueError(
                    "chains must be positive with Pathfinder initialization")
            pf = _pathfinder_init_options(pathfinder_init)
            init_arr = np.empty((opts.chains, self.n_unconstrained))
            err = ctypes.create_string_buffer(4096)
            rc = _lib.stanli_pathfinder_inits(
                self._m, opts.seed, opts.chain_id, opts.chains,
                pf["num_iterations"], pf["num_elbo_draws"],
                pf["history_size"], pf["init_radius"], _dptr(init_arr),
                None, None, err, len(err))
            if rc != 0:
                raise RuntimeError(
                    f"Pathfinder initialization failed: {err.value.decode()}")
            opts.inits = _dptr(init_arr)
        elif inits is not None:
            init_arr = np.ascontiguousarray(inits, dtype=np.float64)
            if init_arr.ndim == 1:
                init_arr = np.ascontiguousarray(
                    np.tile(init_arr, (opts.chains, 1)))
            if init_arr.shape != (opts.chains, self.n_unconstrained):
                raise ValueError(
                    f"inits must be ({opts.chains}, {self.n_unconstrained}) "
                    f"or ({self.n_unconstrained},) on the unconstrained "
                    f"scale, got {init_arr.shape}")
            opts.inits = _dptr(init_arr)

        n_stored = _lib.stanli_n_stored_draws(ctypes.byref(opts))
        raw = np.empty((opts.chains, n_stored, self.n_unconstrained))
        stats = np.empty((opts.chains, n_stored, _N_SAMPLER_COLS))
        reports = (_SampleReport * opts.chains)()
        err = ctypes.create_string_buffer(4096)
        callback_errors = []
        # A typed null function pointer: ctypes does not accept Python None for
        # a CFUNCTYPE argtype, even though both represent a null C callback.
        progress_cb = _SampleProgressCallback()
        if refresh:
            def progress(chain_id, iteration, total, is_warmup, _user):
                if callback_errors:
                    return
                try:
                    _print_sample_progress(chain_id, iteration, total,
                                           is_warmup)
                except BaseException as exc:  # ctypes cannot propagate it
                    callback_errors.append(exc)

            # Keep this object in a local until the native call returns. ctypes
            # callbacks are raw function pointers; collecting it early would
            # leave the sampler calling freed memory.
            progress_cb = _SampleProgressCallback(progress)

        failed = _lib.stanli_sample_multi_progress(
            self._m, ctypes.byref(opts), refresh, _dptr(raw), _dptr(stats),
            progress_cb, None, reports, err, len(err))
        del init_arr
        if failed:
            raise RuntimeError(
                f"{failed} of {opts.chains} chains failed; first: "
                f"{err.value.decode()}")
        if callback_errors:
            raise callback_errors[0]

        names, have_wa = self._column_names()
        out = np.empty((opts.chains, n_stored, len(names)))
        row = np.empty(len(names))
        first_chain = opts.chain_id if opts.chain_id > 0 else 1
        for c in range(opts.chains):
            if have_wa:
                # Generated quantities draw from an RNG stream; give each
                # chain its own, or every chain would produce identical
                # posterior-predictive draws from its own parameters.
                _lib.stanli_wa_seed_chain(
                    self._m, seed & 0xFFFFFFFF, first_chain + c)
            for s in range(n_stored):
                q = np.ascontiguousarray(raw[c, s])
                if have_wa:
                    if _lib.stanli_wa_row(self._m, _dptr(q), _dptr(row)) != 0:
                        raise RuntimeError(
                            f"write_array failed on chain {c}, draw {s}")
                else:
                    _lib.stanli_constrain(self._m, _dptr(q), _dptr(row))
                out[c, s] = row
        fit = Fit(names, out, stats, opts.max_depth, seed, reports)
        if refresh:
            _print_sample_reports(reports, first_chain, opts.samples,
                                  opts.max_depth)
        return fit


# Kept separate from Model's JSON data path: functions bind typed buffers.
from ._function import Function  # noqa: E402
