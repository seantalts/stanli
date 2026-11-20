"""EXPERIMENTAL. Build a Laplace-whitened model from an ordinary one.

See `laplace_whiten`. The builder itself runs in the native library
(stanli_laplace_whiten); this module chooses the latent block and the
starting point, and returns the result as scipy matrices. Needs scipy; the
rest of the package does not.
"""
import ctypes
import time
import warnings
from dataclasses import dataclass, field

import numpy as np

from . import _lib, _dptr, _native_output, _interruptible, _SamplePollCallback
from . import _experimental, _LAPLACE_WHITENING_ADVICE

_PATH_CB = ctypes.CFUNCTYPE(None, ctypes.c_int32, ctypes.c_double,
                            ctypes.c_void_p)
_lib.stanli_run_pathfinder.restype = ctypes.c_int
_lib.stanli_run_pathfinder.argtypes = [
    ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int, ctypes.c_int,
    ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_double),
    ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_double),
    _PATH_CB, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]

_REPAIRS = {"abs": 0, "abs_dense": 1, "clip": 2}


class _Opts(ctypes.Structure):
    _fields_ = [("seed", ctypes.c_uint32), ("max_outer", ctypes.c_int),
                ("inner_iter", ctypes.c_int),
                ("pattern_tolerance", ctypes.c_double),
                ("threshold", ctypes.c_double), ("repair", ctypes.c_int),
                ("clip", ctypes.c_double), ("dense_pattern", ctypes.c_int),
                ("dense_hessian", ctypes.c_int),
                ("keep_precision", ctypes.c_int)]


class _Report(ctypes.Structure):
    _fields_ = [(name, ctypes.c_int64) for name in (
        "n", "n_latent", "n_theta", "nnz", "rank", "n_clipped")] + [
        ("has_spectrum", ctypes.c_int),
        ("min_eigenvalue", ctypes.c_double),
        ("max_eigenvalue", ctypes.c_double)] + [
        (name, ctypes.c_int64) for name in (
            "n_colors", "pattern_gradients", "marginal_gradients",
            "final_gradients", "outer_iterations", "outer_evaluations")] + [
        ("pattern_fallback", ctypes.c_int), ("has_precision", ctypes.c_int)]


_I64 = ctypes.POINTER(ctypes.c_int64)
_F64 = ctypes.POINTER(ctypes.c_double)
_lib.stanli_laplace_whiten.restype = ctypes.c_void_p
_lib.stanli_laplace_whiten.argtypes = [
    ctypes.c_void_p, _F64, ctypes.c_int64, _I64, ctypes.POINTER(_Opts),
    _SamplePollCallback, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int),
    ctypes.c_char_p, ctypes.c_size_t]
_lib.stanli_whitening_get_report.restype = None
_lib.stanli_whitening_get_report.argtypes = [ctypes.c_void_p,
                                             ctypes.POINTER(_Report)]
_lib.stanli_whitening_get_spec.restype = None
_lib.stanli_whitening_get_spec.argtypes = [
    ctypes.c_void_p, _F64, _I64, _I64, _I64, _F64, _F64, _F64, _F64]
_lib.stanli_whitening_get_theta.restype = None
_lib.stanli_whitening_get_theta.argtypes = [ctypes.c_void_p, _F64, _F64]
_lib.stanli_whitening_outer_message.restype = ctypes.c_char_p
_lib.stanli_whitening_outer_message.argtypes = [ctypes.c_void_p]
_lib.stanli_whitening_n_warnings.restype = ctypes.c_int64
_lib.stanli_whitening_n_warnings.argtypes = [ctypes.c_void_p]
_lib.stanli_whitening_warning.restype = ctypes.c_char_p
_lib.stanli_whitening_warning.argtypes = [ctypes.c_void_p, ctypes.c_int64]
_lib.stanli_whitening_get_precision.restype = ctypes.c_int
_lib.stanli_whitening_get_precision.argtypes = [ctypes.c_void_p, _F64]
_lib.stanli_whitening_free.restype = None
_lib.stanli_whitening_free.argtypes = [ctypes.c_void_p]


@dataclass
class LaplaceWhitenReport:
    """What `laplace_whiten` did.

    ``theta_hat`` is the marginal mode of the hyperparameters on the
    unconstrained scale (the entries of ``q_hat`` outside ``latent_index``).
    ``gradients`` counts gradient evaluations by stage, not counting
    Pathfinder, whose cost is ``pathfinder_seconds``. ``nnz_L / n**2`` is the
    fill of the factor. ``rank`` is the number of repaired eigenpairs the
    whitening carries as a low-rank correction (zero unless ``repair="abs"``
    found some). ``n_colors`` is the colour count of the Hessian pattern
    (``n`` for the dense Hessian). ``precision`` is kept only on request.
    """
    n: int
    n_latent: int
    n_theta: int
    latent_index: np.ndarray
    q_hat: np.ndarray
    theta_hat: np.ndarray
    theta_start: np.ndarray
    n_clipped: int
    min_eigenvalue: float
    max_eigenvalue: float
    nnz_L: int
    ordering: str
    perm: np.ndarray
    L: object
    gradients: dict
    pathfinder_seconds: float
    wall_seconds: float
    outer: dict = field(default_factory=dict)
    precision: object = None
    rank: int = 0
    n_colors: int = 0
    hessian: str = ""
    repair: str = ""


def _annotated_latent(model):
    names = [name for name, notes in model.parameter_annotations().items()
             if "latent" in notes]
    if not names:
        raise ValueError(
            "no parameter is annotated @latent; pass latent=[...] or "
            "declare the random effects with @latent, for example "
            "'@latent vector[J] a;'")
    return names


def _block_index(model, latent):
    if isinstance(latent, str) or not hasattr(latent, "__iter__"):
        raise TypeError("latent must be a list of parameter names")
    latent = list(latent)
    if not latent:
        raise ValueError("latent needs at least one parameter name")
    if not all(isinstance(name, str) for name in latent):
        raise TypeError("latent must be a list of parameter names")
    seen = set()
    for name in latent:
        if name in seen:
            raise ValueError(f"latent names {name!r} twice")
        seen.add(name)
    layout = model._unconstrained_layout()
    if not layout:
        raise ValueError("this model is already whitened; build the whitened "
                         "model from the original one")
    start, ranges = 0, {}
    for name, size in layout:
        ranges[name] = (start, start + size)
        start += size
    columns = {c.split("[")[0] for c in model._column_names()}
    for name in latent:
        if name in ranges:
            continue
        what = ("is a transformed parameter or generated quantity, not a "
                "parameter" if name in columns else
                "is not a parameter of this model (data and "
                "undeclared names are not parameters)")
        raise ValueError(f"latent {name!r} {what}; the parameters are "
                         + ", ".join(repr(k) for k, _ in layout))
    index = np.concatenate([np.arange(*ranges[name]) for name in latent])
    return np.sort(index)


def _pathfinder_mean(model, seed, n):
    with model._lock:
        num_draws = max(1000, min(20 * n, 20000, 25_000_000 // n))
        draws = np.empty((num_draws, n))
        lp = np.empty(num_draws)
        lp_approx = np.empty(num_draws)
        summary = np.empty(4)
        err = ctypes.create_string_buffer(4096)
        callback = _PATH_CB(lambda *_: None)
        with _native_output():
            rc = _lib.stanli_run_pathfinder(
                model._handle, seed, 1, num_draws, _dptr(draws), _dptr(lp),
                _dptr(lp_approx), _dptr(summary), callback, None, err,
                len(err))
    if rc != 0:
        raise RuntimeError("Pathfinder failed (" + err.value.decode()
                           + "); pass init= to choose the starting point")
    draws = draws[np.all(np.isfinite(draws), axis=1)]
    if not len(draws):
        raise RuntimeError("Pathfinder returned no finite draws; pass init= "
                           "to choose the starting point")
    return draws.mean(0)


def _run(model, q_start, iu, opts):
    """The native builder's result as numpy arrays and its warnings."""
    err = ctypes.create_string_buffer(4096)
    handle = None

    def call(poll, interrupted):
        nonlocal handle
        handle = _lib.stanli_laplace_whiten(
            model._handle, _dptr(q_start), len(iu), iu.ctypes.data_as(_I64),
            ctypes.byref(opts), poll, None, interrupted, err, len(err))
        return 0

    with model._lock:
        _interruptible(call)
    if not handle:
        raise RuntimeError("laplace_whiten: "
                           + err.value.decode(errors="replace"))
    try:
        rep = _Report()
        _lib.stanli_whitening_get_report(handle, ctypes.byref(rep))
        n, k = rep.n, rep.rank
        out = dict(q_hat=np.empty(n), perm=np.empty(n, dtype=np.int64),
                   col_ptr=np.empty(n + 1, dtype=np.int64),
                   row_idx=np.empty(rep.nnz, dtype=np.int64),
                   values=np.empty(rep.nnz), c=np.empty(n),
                   b=np.empty((n, k)), w=np.empty((n, k)),
                   theta_start=np.empty(rep.n_theta),
                   theta_hat=np.empty(rep.n_theta), precision=None)
        _lib.stanli_whitening_get_spec(
            handle, _dptr(out["q_hat"]), out["perm"].ctypes.data_as(_I64),
            out["col_ptr"].ctypes.data_as(_I64),
            out["row_idx"].ctypes.data_as(_I64), _dptr(out["values"]),
            _dptr(out["c"]), _dptr(out["b"]), _dptr(out["w"]))
        _lib.stanli_whitening_get_theta(handle, _dptr(out["theta_start"]),
                                        _dptr(out["theta_hat"]))
        if rep.has_precision:
            out["precision"] = np.empty((n, n))
            _lib.stanli_whitening_get_precision(handle,
                                                _dptr(out["precision"]))
        out["message"] = _lib.stanli_whitening_outer_message(handle).decode()
        out["warnings"] = [
            _lib.stanli_whitening_warning(handle, i).decode()
            for i in range(_lib.stanli_whitening_n_warnings(handle))]
    finally:
        _lib.stanli_whitening_free(handle)
    return rep, out


def laplace_whiten(model, latent=None, *, init=None, seed=1, max_outer=60,
                   inner_iter=2000, pattern_tolerance=1e-6,
                   threshold=1e-6, repair="abs", clip=None,
                   hessian="coloured", keep_precision=False,
                   pattern="probe"):
    """EXPERIMENTAL. Whiten `model` around its Laplace-approximate marginal
    mode and return ``(whitened_model, report)``.

    `latent` lists the parameters (Stan names) that make up the random
    effects u; every other parameter is a hyperparameter theta. Within the
    unconstrained vector the latent block is those parameters' entries, in
    declaration order, so a simplex of K entries contributes K - 1.

    With `latent=None` the latent parameters are the ones declared `@latent`
    (`model.parameter_annotations()`), and a model with none raises. A
    `latent` list wins over the annotations when both are present.

    Steps, with the harness' fixed thresholds as defaults:

    1. Start at the Pathfinder mean (or at the unconstrained `init`) and
       take the sparsity pattern of the Hessian block over the latent
       coordinates there, entries above `pattern_tolerance` relative to the
       square root of the diagonal. `pattern="probe"`, the default, finds it
       without the dense Hessian: random-weight gradient differences along
       groups of coordinates give a superset (a missed nonzero would need a
       cancellation of probability zero), the coloured Hessian over that
       superset is thresholded, and three random central differences of the
       gradient check that nothing was missed. If a check fails the builder
       warns and uses the dense pattern. On a hierarchical model it costs a
       few hundred gradients and O(nnz) memory however many groups there
       are. `pattern="dense"` forms the dense Hessian, 2 n + 1 gradients and
       8 n^2 bytes.
    2. Maximize over theta the marginal
       ``log p(theta, u_hat(theta)) - 1/2 log det(-H_uu)``. u_hat comes from
       an inner L-BFGS over u, H_uu from central differences of the gradient
       along the pattern (one gradient pair per colour of its distance-2
       colouring), its log determinant from a sparse factorization, theta
       from an outer L-BFGS with finite differences.
    3. At the result take Q = -H and drop entries below `threshold`
       relative to ``sqrt(Q_ii Q_jj)``. `hessian="coloured"` (the default)
       differences gradients along the same colouring, at 2 (colours +
       hyperparameters) gradients and sparse storage; `hessian="dense"`
       calls `log_prob_hessian` at 2 n + 1 gradients.
    4. Factor Q with a sparse LDL^T under an approximate minimum degree
       ordering. If Q is not positive definite, repair its eigenvalues and
       warn. `repair="abs"`, the default and what the benchmark measured,
       replaces each eigenvalue below `clip` (default 1e-8) times the
       largest absolute eigenvalue by its absolute value, floored at that
       level. It does this without densifying: the few such eigenpairs
       (lambda_i, v_i) come from a Lanczos search, Q' = Q + V diag(delta)
       V^T with delta_i = max(|lambda_i|, floor) - lambda_i, and the
       whitening applies the Cholesky factor of ``D + W W^T`` (W = L^{-1}
       P V sqrt(delta)) to the sparse LDL^T factor, so the whitened
       coordinates have exactly the precision Q'. Per-gradient cost is
       O(nnz(L) + n k) for k repaired eigenvalues.
       `repair="abs_dense"` does the same with a dense eigendecomposition and
       a dense Cholesky of the repaired matrix (O(n^3), and a dense
       factor sparsified afterwards). `repair="clip"` raises every
       eigenvalue below `clip` (default 1e-3) times the largest to that
       value, which can make a negative-curvature direction much wider than
       the posterior; it is dense too.
    5. Call ``model.whiten(q_hat, perm, L, lowrank=...)``.

    Run the whitened model with ``metric="unit_e"`` and step-size-only
    adaptation (``adapt_metric=False``); a short warmup is enough. Needs
    scipy.

    `init` is an unconstrained starting point that replaces Pathfinder.
    `keep_precision` stores the repaired Q' on the report as a dense array.
    """
    _experimental("Laplace whitening", _LAPLACE_WHITENING_ADVICE)
    import scipy.sparse as sp
    t0 = time.time()
    if repair not in _REPAIRS:
        raise ValueError('repair must be "abs", "abs_dense" or "clip"')
    if hessian not in ("coloured", "dense"):
        raise ValueError('hessian must be "coloured" or "dense"')
    if pattern not in ("probe", "dense"):
        raise ValueError('pattern must be "probe" or "dense"')
    if clip is None:
        clip = 1e-3 if repair == "clip" else 1e-8
    if latent is None:
        latent = _annotated_latent(model)
    iu = _block_index(model, latent)
    n = model.n_unconstrained
    with model._lock:
        model._prepare_run(seed)
    pathfinder_seconds = 0.0
    if init is None:
        t = time.time()
        q_start = _pathfinder_mean(model, seed, n)
        pathfinder_seconds = time.time() - t
    else:
        q_start = np.ascontiguousarray(init, dtype=np.float64)
        if q_start.shape != (n,):
            raise ValueError(f"init has shape {q_start.shape}, the model has "
                             f"{n} unconstrained parameters")
    opts = _Opts(seed=seed, max_outer=max_outer, inner_iter=inner_iter,
                 pattern_tolerance=pattern_tolerance, threshold=threshold,
                 repair=_REPAIRS[repair], clip=clip,
                 dense_pattern=pattern == "dense",
                 dense_hessian=hessian == "dense",
                 keep_precision=bool(keep_precision))
    rep, out = _run(model, q_start, np.ascontiguousarray(iu, dtype=np.int64),
                    opts)
    for message in out["warnings"]:
        warnings.warn(message, RuntimeWarning, stacklevel=2)
    L = sp.csc_matrix((out["values"], out["row_idx"], out["col_ptr"]),
                      shape=(n, n))
    lowrank = (out["c"], out["b"], out["w"]) if rep.rank else None
    whitened = model.whiten(out["q_hat"], out["perm"], L, lowrank=lowrank)
    spectrum = rep.has_spectrum
    gradients = {"pattern_hessian": rep.pattern_gradients,
                 "marginal_mode": rep.marginal_gradients,
                 "final_hessian": rep.final_gradients}
    gradients["total"] = sum(gradients.values())
    outer = {}
    if rep.n_theta:
        outer = dict(iterations=rep.outer_iterations,
                     evaluations=rep.outer_evaluations,
                     message=out["message"])
    return whitened, LaplaceWhitenReport(
        n=n, n_latent=len(iu), n_theta=n - len(iu), latent_index=iu,
        q_hat=out["q_hat"], theta_hat=out["theta_hat"],
        theta_start=out["theta_start"], n_clipped=rep.n_clipped,
        min_eigenvalue=rep.min_eigenvalue if spectrum else None,
        max_eigenvalue=rep.max_eigenvalue if spectrum else None,
        nnz_L=rep.nnz, ordering="amd", perm=out["perm"], L=L,
        gradients=gradients, pathfinder_seconds=pathfinder_seconds,
        wall_seconds=time.time() - t0, outer=outer,
        precision=out["precision"], rank=rep.rank, n_colors=rep.n_colors,
        hessian=hessian, repair=repair)
