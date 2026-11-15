"""EXPERIMENTAL. Build a Laplace-whitened model from an ordinary one.

See `laplace_whiten`. Needs scipy; the rest of the package does not.
"""
import ctypes
import time
import warnings
from dataclasses import dataclass, field

import numpy as np

from . import _lib, _dptr, _native_output

_PATH_CB = ctypes.CFUNCTYPE(None, ctypes.c_int32, ctypes.c_double,
                            ctypes.c_void_p)
_lib.stanli_run_pathfinder.restype = ctypes.c_int
_lib.stanli_run_pathfinder.argtypes = [
    ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int, ctypes.c_int,
    ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_double),
    ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_double),
    _PATH_CB, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]


@dataclass
class LaplaceWhitenReport:
    """What `laplace_whiten` did.

    ``theta_hat`` is the marginal mode of the hyperparameters on the
    unconstrained scale (the entries of ``q_hat`` outside ``latent_index``).
    ``gradients`` counts gradient evaluations by stage, not counting
    Pathfinder, whose cost is ``pathfinder_seconds``. ``nnz_L / n**2`` is the
    fill of the factor. ``precision`` is kept only on request.
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
    columns = {c.split("[")[0] for c in model._column_names()[0]}
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
    model._prepare_run(seed, 1)
    num_draws = max(1000, min(20 * n, 20000))
    draws = np.empty((num_draws, n))
    lp = np.empty(num_draws)
    lp_approx = np.empty(num_draws)
    summary = np.empty(4)
    err = ctypes.create_string_buffer(4096)
    callback = _PATH_CB(lambda *_: None)
    with _native_output():
        rc = _lib.stanli_run_pathfinder(
            model._handle, seed, 1, num_draws, _dptr(draws), _dptr(lp),
            _dptr(lp_approx), _dptr(summary), callback, None, err, len(err))
    if rc != 0:
        raise RuntimeError("Pathfinder failed (" + err.value.decode()
                           + "); pass init= to choose the starting point")
    draws = draws[np.all(np.isfinite(draws), axis=1)]
    if not len(draws):
        raise RuntimeError("Pathfinder returned no finite draws; pass init= "
                           "to choose the starting point")
    return draws.mean(0)


def _pattern(H, tol):
    d = np.sqrt(np.abs(np.diag(H)))
    scale = np.outer(d, d)
    scale[scale == 0] = np.abs(H).max()
    P = (np.abs(H) > tol * scale) | np.eye(H.shape[0], dtype=bool)
    return P | P.T


def _color_columns(P):
    n = P.shape[0]
    rows_of = [np.nonzero(P[:, j])[0] for j in range(n)]
    cols_of_row = [np.nonzero(P[i, :])[0] for i in range(n)]
    color = -np.ones(n, dtype=int)
    for j in np.argsort(-P.sum(0)):
        used = set()
        for i in rows_of[j]:
            used.update(color[cols_of_row[i]].tolist())
        c = 0
        while c in used:
            c += 1
        color[j] = c
    return color


def _colored_hessian(grad, q, P, cols):
    """Central-difference Hessian of the log density over `cols`, using one
    pair of gradients per color of the sparsity pattern P (indexed in cols)."""
    k = len(cols)
    color = _color_columns(P)
    H = np.zeros((k, k))
    for c in range(color.max() + 1):
        js = np.nonzero(color == c)[0]
        e = np.zeros(q.size)
        step = 1e-4 * np.maximum(1.0, np.abs(q[cols[js]]))
        e[cols[js]] = step
        gp = grad(q + e)[1]
        gm = grad(q - e)[1]
        dg = (gp - gm)[cols]
        for jj, j in enumerate(js):
            rows = np.nonzero(P[:, j])[0]
            H[rows, j] = dg[rows] / (2 * step[jj])
    return (H + H.T) / 2


def _marginal_mode(model, q_start, iu, it, pattern_uu, max_outer, inner_iter,
                   counter):
    import scipy.optimize as so
    n = q_start.size

    def grad(q):
        counter[0] += 1
        return model.log_prob_grad(q)

    cache = {"u": q_start[iu].copy()}

    def inner(theta):
        q = np.empty(n)
        q[it] = theta

        def f(u):
            q[iu] = u
            try:
                lp, g = grad(q)
            except RuntimeError:
                return 1e300, np.zeros(len(iu))
            if not np.isfinite(lp):
                return 1e300, np.zeros(len(iu))
            return -lp, -g[iu]
        r = so.minimize(f, cache["u"], jac=True, method="L-BFGS-B",
                        options=dict(maxiter=inner_iter, gtol=1e-8,
                                     ftol=1e-14, maxcor=20))
        q[iu] = r.x
        return q, -r.fun

    def neg_marginal(theta):
        q, lp = inner(theta)
        if not np.isfinite(lp) or lp < -1e299:
            return 1e300
        cache["u"] = q[iu].copy()
        H = _colored_hessian(grad, q, pattern_uu, iu)
        try:
            c = np.linalg.cholesky(-H)
        except np.linalg.LinAlgError:
            return 1e300
        return -(lp - np.sum(np.log(np.diag(c))))

    theta0 = q_start[it].copy()
    outer = {}
    if len(it):
        r = so.minimize(neg_marginal, theta0, method="L-BFGS-B",
                        options=dict(maxiter=max_outer, eps=1e-5, ftol=1e-10))
        theta = r.x
        outer = dict(iterations=int(r.nit), evaluations=int(r.nfev),
                     message=str(r.message))
    else:
        theta = theta0
    q, _ = inner(theta)
    return q, theta0, theta, outer


def _repair(Q, floor, mode="clip"):
    """Q itself when it is positive definite; otherwise its eigenvalues below
    floor * (largest) are raised to that ("clip") or replaced by their
    absolute values with the same floor ("abs"). Returns Q, count, min, max."""
    try:
        np.linalg.cholesky(Q)
    except np.linalg.LinAlgError:
        pass
    else:
        return Q, 0, None, None
    w, V = np.linalg.eigh(Q)
    if mode == "abs":
        a = np.abs(w)
        lo = floor * a.max()
        w2 = np.maximum(a, lo)
        count = int((w2 != w).sum())
    else:
        lo = floor * w.max()
        if not lo > 0:
            raise RuntimeError("the negative Hessian has no positive "
                               "eigenvalue; the starting point is not near a "
                               "mode")
        w2 = np.maximum(w, lo)
        count = int((w < lo).sum())
    repaired = (V * w2) @ V.T
    return (repaired + repaired.T) / 2, count, float(w.min()), float(w.max())


def _fill_reducing_order(pattern):
    """The permutation of the symmetric pattern with the sparsest Cholesky
    factor among minimum degree and reverse Cuthill-McKee."""
    import scipy.sparse as sp
    import scipy.sparse.csgraph as csgraph
    import scipy.sparse.linalg as spla
    n = pattern.shape[0]
    spd = sp.csc_matrix(pattern.astype(float) + n * np.eye(n))
    candidates = {}
    perm_c = spla.splu(spd, permc_spec="MMD_AT_PLUS_A", diag_pivot_thresh=0,
                       options=dict(SymmetricMode=True)).perm_c
    candidates["minimum_degree"] = np.argsort(perm_c)
    candidates["rcm"] = csgraph.reverse_cuthill_mckee(
        sp.csr_matrix(pattern), symmetric_mode=True)
    dense = spd.toarray()
    best = None
    for name, perm in candidates.items():
        factor = np.linalg.cholesky(dense[np.ix_(perm, perm)])
        nnz = int((np.abs(factor) > 1e-13 * np.abs(factor).max()).sum())
        if best is None or nnz < best[0]:
            best = (nnz, name, np.asarray(perm, dtype=np.int64))
    return best[1], best[2]


def laplace_whiten(model, latent, *, init=None, seed=1, max_outer=60,
                   inner_iter=2000, pattern_tolerance=1e-6,
                   threshold=1e-6, repair="abs", clip=None,
                   keep_precision=False):
    """EXPERIMENTAL. Whiten `model` around its Laplace-approximate marginal
    mode and return ``(whitened_model, report)``.

    `latent` lists the parameters (Stan names) that make up the random
    effects u; every other parameter is a hyperparameter theta. Within the
    unconstrained vector the latent block is those parameters' entries, in
    declaration order, so a simplex of K entries contributes K - 1.

    Steps, with the harness' fixed thresholds as defaults:

    1. Start at the Pathfinder mean (or at the unconstrained `init`) and
       take the sparsity pattern of the Hessian there, entries above
       `pattern_tolerance` relative to the square root of the diagonal.
    2. Maximize over theta the marginal
       ``log p(theta, u_hat(theta)) - 1/2 log det(-H_uu)``. u_hat comes from
       an inner L-BFGS over u, H_uu from central differences of the gradient
       along the pattern, theta from an outer L-BFGS with finite differences.
    3. At the result take Q = -H, the full Hessian from `log_prob_hessian`,
       and drop entries below `threshold` relative to ``sqrt(Q_ii Q_jj)``.
    4. If Q is not positive definite, repair its eigenvalues and warn.
       `repair="abs"`, the default and what the benchmark measured,
       replaces each eigenvalue by its absolute value, floored at `clip`
       (default 1e-8) times the largest. It keeps the scale of a direction
       whose curvature came out negative. `repair="clip"` raises every
       eigenvalue below `clip` (default 1e-3) times the largest to that
       value, which can make such a direction much wider than the
       posterior.
    5. Order with a fill-reducing permutation, factor the permuted matrix
       densely, keep the nonzeros as a sparse lower factor L, and call
       ``model.whiten(q_hat, perm, L)``.

    Run the whitened model with ``metric="unit_e"`` and step-size-only
    adaptation (``adapt_metric=False``); a short warmup is enough. Sampling
    cost is one extra pair of sparse triangular solves per gradient. Needs
    scipy. Models with n in the low thousands are the intended range: the
    Hessian, eigendecomposition and factorization are dense.

    `init` is an unconstrained starting point that replaces Pathfinder.
    `keep_precision` stores the repaired Q on the report as a dense array.
    """
    import scipy.sparse as sp
    t0 = time.time()
    if repair not in ("clip", "abs"):
        raise ValueError('repair must be "clip" or "abs"')
    if clip is None:
        clip = 1e-3 if repair == "clip" else 1e-8
    iu = _block_index(model, latent)
    n = model.n_unconstrained
    it = np.setdiff1d(np.arange(n), iu)
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
    _, _, H_start = model.log_prob_hessian(q_start)
    if not np.all(np.isfinite(H_start)):
        raise RuntimeError("the Hessian at the starting point is not finite")
    pattern_uu = _pattern(H_start, pattern_tolerance)[np.ix_(iu, iu)]
    counter = [0]
    q_hat, theta_start, theta_hat, outer = _marginal_mode(
        model, q_start, iu, it, pattern_uu, max_outer, inner_iter, counter)
    marginal_grads = counter[0]

    _, _, H = model.log_prob_hessian(q_hat)
    Q = -(H + H.T) / 2
    if not np.all(np.isfinite(Q)):
        raise RuntimeError("the Hessian at the marginal mode is not finite")
    d = np.sqrt(np.abs(np.diag(Q)))
    scale = np.outer(d, d)
    Q = np.where(np.abs(Q) >= threshold * scale, Q, 0.0)
    pattern = Q != 0
    Qr, n_clipped, min_w, max_w = _repair(Q, clip, repair)
    if n_clipped:
        how = ("clipped" if repair == "clip"
               else "replaced by absolute values")
        warnings.warn(
            f"laplace_whiten: {n_clipped} of {n} eigenvalues of the negative "
            f"Hessian were {how} (floor {clip:g} times the largest; "
            f"smallest {min_w:.3g}, largest {max_w:.3g}); the whitened "
            "coordinates are only approximately standard normal",
            RuntimeWarning, stacklevel=2)
    name, perm = _fill_reducing_order(pattern)
    factor = np.linalg.cholesky(Qr[np.ix_(perm, perm)])
    keep = np.abs(factor) > 1e-13 * np.abs(factor).max()
    np.fill_diagonal(keep, True)
    L = sp.csc_matrix(np.where(keep, factor, 0.0))
    whitened = model.whiten(q_hat, perm, L)
    gradients = {"pattern_hessian": 1 + 2 * n,
                 "marginal_mode": marginal_grads,
                 "final_hessian": 1 + 2 * n}
    gradients["total"] = sum(gradients.values())
    return whitened, LaplaceWhitenReport(
        n=n, n_latent=len(iu), n_theta=len(it), latent_index=iu, q_hat=q_hat,
        theta_hat=theta_hat, theta_start=theta_start, n_clipped=n_clipped,
        min_eigenvalue=min_w, max_eigenvalue=max_w, nnz_L=int(L.nnz),
        ordering=name, perm=perm, L=L, gradients=gradients,
        pathfinder_seconds=pathfinder_seconds, wall_seconds=time.time() - t0,
        outer=outer, precision=Qr if keep_precision else None)
