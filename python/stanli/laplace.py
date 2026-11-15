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


_I64 = ctypes.POINTER(ctypes.c_int64)
_F64 = ctypes.POINTER(ctypes.c_double)
_lib.stanli_sparse_factor_new.restype = ctypes.c_void_p
_lib.stanli_sparse_factor_new.argtypes = [
    ctypes.c_int64, _I64, _I64, _F64, _I64, ctypes.c_char_p, ctypes.c_size_t]
_lib.stanli_sparse_factor_nnz.restype = ctypes.c_int64
_lib.stanli_sparse_factor_nnz.argtypes = [ctypes.c_void_p]
_lib.stanli_sparse_factor_stats.restype = None
_lib.stanli_sparse_factor_stats.argtypes = [
    ctypes.c_void_p, _I64, _F64]
_lib.stanli_sparse_factor_get.restype = None
_lib.stanli_sparse_factor_get.argtypes = [
    ctypes.c_void_p, _I64, _I64, _I64, _F64, _F64]
_lib.stanli_sparse_factor_lowrank.restype = ctypes.c_int
_lib.stanli_sparse_factor_lowrank.argtypes = [
    ctypes.c_void_p, ctypes.c_int64, _F64, _F64, _F64, _F64,
    ctypes.c_char_p, ctypes.c_size_t]
_lib.stanli_sparse_factor_free.restype = None
_lib.stanli_sparse_factor_free.argtypes = [ctypes.c_void_p]


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
    """The symmetric sparsity pattern (CSC, with the diagonal) of the entries
    of the dense matrix H above `tol` relative to sqrt(|H_ii| |H_jj|)."""
    import scipy.sparse as sp
    n = H.shape[0]
    d = np.sqrt(np.abs(np.diag(H)))
    fallback = np.abs(H).max()
    blocks = []
    for start in range(0, n, 512):
        rows = slice(start, min(n, start + 512))
        scale = np.outer(d[rows], d)
        scale[scale == 0] = fallback
        blocks.append(sp.csr_matrix(np.abs(H[rows]) > tol * scale))
    P = sp.vstack(blocks, format="csr")
    P = P + sp.identity(n, dtype=bool, format="csr")
    P = (P + P.T).astype(bool)
    P.sum_duplicates()
    return P.tocsc()


def _distance2_colouring(pattern):
    """Greedy colouring of the columns of a symmetric sparsity pattern so
    that columns of one colour have no row in common (distance-2 colouring
    of its graph); most-connected columns first."""
    A = pattern.tocsr()
    n = A.shape[0]
    indptr, indices = A.indptr, A.indices
    color = -np.ones(n, dtype=np.int64)
    for j in np.argsort(-np.diff(indptr), kind="stable"):
        used = set()
        for i in indices[indptr[j]:indptr[j + 1]]:
            used.update(color[indices[indptr[i]:indptr[i + 1]]].tolist())
        c = 0
        while c in used:
            c += 1
        color[j] = c
    return color


class _HessianPlan:
    def __init__(self, pattern, dense, hub_degree):
        m = pattern.shape[0]
        degree = np.diff(pattern.indptr)
        limit = max(64, int(4 * np.sqrt(m))) if hub_degree is None \
            else hub_degree
        dense = np.union1d(np.asarray(dense, dtype=np.int64),
                           np.nonzero(degree > limit)[0]).astype(np.int64)
        free = np.setdiff1d(np.arange(m), dense)
        self.pattern = pattern.tocsc()
        self.dense = dense
        self.free = free
        sub = self.pattern[free][:, free] if len(free) else None
        self.color = (_distance2_colouring(sub) if len(free)
                      else np.zeros(0, dtype=np.int64))
        self.n_colors = int(self.color.max()) + 1 if len(free) else 0
        self.gradients = 2 * (self.n_colors + len(dense))


def _sparse_hessian(grad, q, pattern, cols, dense=(), plan=None,
                    hub_degree=None):
    """Central-difference Hessian of the log density over the coordinates
    `cols` of q, as a symmetric sparse matrix.

    `pattern` is the symmetric sparsity pattern in `cols` (CSC with the
    diagonal) and grad(q) returns (lp, gradient). Columns of one colour of the
    pattern are perturbed together, one pair of gradients per colour. The
    `dense` columns, and any column with unusually many entries, are
    perturbed one at a time and give their rows by symmetry; this keeps a
    hyperparameter coupled to every random effect from forcing one colour per
    column. Costs 2 (colours + dense columns) gradients."""
    import scipy.sparse as sp
    cols = np.asarray(cols, dtype=np.int64)
    if plan is None:
        plan = _HessianPlan(pattern, dense, hub_degree)
    P = plan.pattern
    m = len(cols)
    step = 1e-4 * np.maximum(1.0, np.abs(q[cols]))
    in_free = np.zeros(m, dtype=bool)
    in_free[plan.free] = True

    def difference(js):
        e = np.zeros(q.size)
        e[cols[js]] = step[js]
        gp = grad(q + e)[1]
        gm = grad(q - e)[1]
        return (gp - gm)[cols]

    fr, fc, fv = [], [], []
    for c in range(plan.n_colors):
        js = plan.free[plan.color == c]
        dg = difference(js)
        for j in js:
            rows = P.indices[P.indptr[j]:P.indptr[j + 1]]
            rows = rows[in_free[rows]]
            fr.append(rows)
            fc.append(np.full(len(rows), j))
            fv.append(dg[rows] / (2 * step[j]))
    hr, hc, hv = [], [], []
    for h in plan.dense:
        dg = difference(np.array([h]))
        rows = np.arange(m)
        hr.append(rows)
        hc.append(np.full(m, h))
        hv.append(dg / (2 * step[h]))

    def build(r, c, v):
        if not r:
            return sp.csc_matrix((m, m))
        return sp.csc_matrix((np.concatenate(v),
                              (np.concatenate(r), np.concatenate(c))),
                             shape=(m, m))

    F = build(fr, fc, fv)
    D = build(hr, hc, hv)
    dense_mask = ~in_free
    Dfh = D.multiply(in_free[:, None].astype(float)).tocsc()
    Dhh = D.multiply(dense_mask[:, None].astype(float)).tocsc()
    H = (F + F.T) / 2 + Dfh + Dfh.T + (Dhh + Dhh.T) / 2
    return sp.csc_matrix(H)


class _Factor:
    """A sparse LDL^T factorization from the native library:
    ``P A P^T = L diag(d) L^T`` with ``L`` unit lower triangular (scipy
    CSC), ``P v = v[perm]``."""

    def __init__(self, handle, n):
        import scipy.sparse as sp
        self._handle = handle
        self.n = n
        nnz = _lib.stanli_sparse_factor_nnz(handle)
        self.perm = np.empty(n, dtype=np.int64)
        col_ptr = np.empty(n + 1, dtype=np.int64)
        row_idx = np.empty(nnz, dtype=np.int64)
        values = np.empty(nnz)
        self.d = np.empty(n)
        _lib.stanli_sparse_factor_get(
            handle, self.perm.ctypes.data_as(_I64),
            col_ptr.ctypes.data_as(_I64), row_idx.ctypes.data_as(_I64),
            _dptr(values), _dptr(self.d))
        self.L = sp.csc_matrix((values, row_idx, col_ptr), shape=(n, n))
        negative = ctypes.c_int64()
        logdet = ctypes.c_double()
        _lib.stanli_sparse_factor_stats(handle, ctypes.byref(negative),
                                        ctypes.byref(logdet))
        self.n_negative = int(negative.value)
        self.logabsdet = float(logdet.value)
        self.nnz = int(nnz)

    def __del__(self):
        handle, self._handle = getattr(self, "_handle", None), None
        if handle:
            _lib.stanli_sparse_factor_free(handle)

    def lowrank(self, U):
        """(c, B, W) for ``diag(d) + W0 W0^T`` with ``W0 = L^{-1} P U``, in
        the form `Model.whiten(lowrank=...)` takes. U is n by k in the
        matrix's own ordering."""
        U = np.asfortranarray(U, dtype=np.float64)
        k = U.shape[1]
        c = np.empty(self.n)
        B = np.empty((self.n, k))
        W = np.empty((self.n, k))
        err = ctypes.create_string_buffer(4096)
        rc = _lib.stanli_sparse_factor_lowrank(
            self._handle, k, _dptr(U), _dptr(c), _dptr(B), _dptr(W), err,
            len(err))
        if rc != 0:
            raise RuntimeError(err.value.decode())
        return c, B, W


def _ldlt(A, perm=None):
    """Sparse LDL^T of the symmetric matrix whose lower triangle is that of
    the scipy sparse matrix A. `perm` is the ordering to use; the default is
    approximate minimum degree."""
    import scipy.sparse as sp
    A = sp.tril(sp.csc_matrix(A), format="csc")
    A.sum_duplicates()
    A.sort_indices()
    n = A.shape[0]
    col_ptr = np.ascontiguousarray(A.indptr, dtype=np.int64)
    row_idx = np.ascontiguousarray(A.indices, dtype=np.int64)
    values = np.ascontiguousarray(A.data, dtype=np.float64)
    if perm is not None:
        perm = np.ascontiguousarray(perm, dtype=np.int64)
    err = ctypes.create_string_buffer(4096)
    handle = _lib.stanli_sparse_factor_new(
        n, col_ptr.ctypes.data_as(_I64), row_idx.ctypes.data_as(_I64),
        _dptr(values), None if perm is None else perm.ctypes.data_as(_I64),
        err, len(err))
    if not handle:
        message = err.value.decode()
        raise (RuntimeError if "pivot" in message else ValueError)(message)
    return _Factor(handle, n)


def _marginal_mode(model, q_start, iu, it, pattern_uu, max_outer, inner_iter,
                   counter):
    import scipy.optimize as so
    n = q_start.size

    def grad(q):
        counter[0] += 1
        return model.log_prob_grad(q)

    plan = _HessianPlan(pattern_uu, (), None)
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
        H = _sparse_hessian(grad, q, pattern_uu, iu, plan=plan)
        try:
            f = _ldlt(-H)
        except RuntimeError:
            return 1e300
        if f.n_negative:
            return 1e300
        return -(lp - 0.5 * f.logabsdet)

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
    absolute values with the same floor ("abs"). Dense. Returns Q, count,
    min, max."""
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


def _bad_eigenpairs(Q, floor, n_negative):
    """Eigenvalues of the symmetric sparse Q below `floor` times the largest
    absolute eigenvalue, with their eigenvectors; also the extreme
    eigenvalues. `n_negative` is the exact count of negative eigenvalues
    (from the inertia of an LDL^T), which the Lanczos search must reach."""
    import scipy.sparse.linalg as spla
    n = Q.shape[0]
    if n <= 64:
        w, V = np.linalg.eigh(Q.toarray())
        lo = floor * np.abs(w).max()
        bad = w < lo
        return w[bad], V[:, bad], float(w.min()), float(w.max())
    top = float(spla.eigsh(Q, k=1, which="LA", return_eigenvectors=False)[0])
    k = min(max(2 * n_negative, n_negative + 4), n - 2)
    for attempt in range(8):
        w, V = spla.eigsh(Q, k=k, which="SA", ncv=min(n - 1, max(2 * k + 1, 20)),
                          v0=np.random.RandomState(attempt).randn(n))
        order = np.argsort(w)
        w, V = w[order], V[:, order]
        lo = floor * max(abs(top), abs(w[0]))
        bad = w < lo
        found = int((w < 0).sum())
        if found >= n_negative and (bad.sum() < k or k >= n - 2):
            return w[bad], V[:, bad], float(w[0]), top
        k = min(2 * k, n - 2)
    raise RuntimeError(
        f"could not find all {n_negative} negative eigenvalues of the "
        'negative Hessian; use repair="abs_dense"')


def _dense_whiten(model, q_hat, Q, pattern, repair, clip, n_clipped_warning):
    import scipy.sparse as sp
    Qr, n_clipped, min_w, max_w = _repair(Q, clip, repair)
    if n_clipped:
        n_clipped_warning(n_clipped, min_w, max_w)
    name, perm = _fill_reducing_order(pattern)
    factor = np.linalg.cholesky(Qr[np.ix_(perm, perm)])
    keep = np.abs(factor) > 1e-13 * np.abs(factor).max()
    np.fill_diagonal(keep, True)
    L = sp.csc_matrix(np.where(keep, factor, 0.0))
    return (model.whiten(q_hat, perm, L), L, perm, name, Qr, n_clipped,
            min_w, max_w, 0)


def _sparse_whiten(model, q_hat, Q, floor, keep_precision, n_clipped_warning):
    import scipy.sparse as sp
    try:
        f = _ldlt(Q)
    except RuntimeError as e:
        raise RuntimeError(f"{e}; use repair=\"abs_dense\"") from None
    n = Q.shape[0]
    ratio = floor if floor > 0 else 1e-8
    fine = f.n_negative == 0 and f.d.min() > ratio * f.d.max()
    n_clipped, min_w, max_w, rank = 0, None, None, 0
    lowrank, U = None, None
    if fine:
        L = (f.L @ sp.diags(np.sqrt(f.d))).tocsc()
    else:
        w, V, min_w, max_w = _bad_eigenpairs(Q, floor, f.n_negative)
        n_clipped = len(w)
        if n_clipped:
            lo = floor * max(abs(min_w), abs(max_w))
            delta = np.maximum(np.abs(w), lo) - w
            U = V * np.sqrt(delta)
            n_clipped_warning(n_clipped, min_w, max_w)
            try:
                lowrank = f.lowrank(U)
            except RuntimeError as e:
                raise RuntimeError(
                    f"{e}; the unpivoted factorization of the indefinite "
                    'negative Hessian lost accuracy: use repair="abs_dense"'
                ) from None
            L = f.L
            rank = U.shape[1]
        else:
            L = (f.L @ sp.diags(np.sqrt(f.d))).tocsc()
    L = sp.csc_matrix(L)
    L.sort_indices()
    whitened = model.whiten(q_hat, f.perm, L, lowrank=lowrank)
    precision = None
    if keep_precision:
        precision = Q.toarray()
        if U is not None:
            precision = precision + U @ U.T
    return (whitened, L, f.perm, "amd", precision, n_clipped, min_w, max_w,
            rank)


def laplace_whiten(model, latent, *, init=None, seed=1, max_outer=60,
                   inner_iter=2000, pattern_tolerance=1e-6,
                   threshold=1e-6, repair="abs", clip=None,
                   hessian="coloured", keep_precision=False):
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
       This one Hessian is dense, so it needs 8 n^2 bytes and 2 n + 1
       gradients.
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
    import scipy.sparse as sp
    t0 = time.time()
    if repair not in ("clip", "abs", "abs_dense"):
        raise ValueError('repair must be "abs", "abs_dense" or "clip"')
    if hessian not in ("coloured", "dense"):
        raise ValueError('hessian must be "coloured" or "dense"')
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
    pattern_full = _pattern(H_start, pattern_tolerance)
    del H_start
    pattern_uu = pattern_full[iu][:, iu]
    counter = [0]
    q_hat, theta_start, theta_hat, outer = _marginal_mode(
        model, q_start, iu, it, pattern_uu, max_outer, inner_iter, counter)
    marginal_grads = counter[0]

    if hessian == "dense":
        _, _, H = model.log_prob_hessian(q_hat)
        H = sp.csc_matrix(H)
        final_grads = 1 + 2 * n
        n_colors = n
    else:
        counter[0] = 0

        def grad(q):
            counter[0] += 1
            return model.log_prob_grad(q)

        plan = _HessianPlan(pattern_full, it, None)
        H = _sparse_hessian(grad, q_hat, pattern_full, np.arange(n),
                            plan=plan)
        final_grads = counter[0]
        n_colors = plan.n_colors
    if not np.all(np.isfinite(H.data)):
        raise RuntimeError("the Hessian at the marginal mode is not finite")
    Q = -(H + H.T) / 2
    Q = sp.csc_matrix(Q)
    d = np.sqrt(np.abs(Q.diagonal()))
    coo = Q.tocoo()
    scale = d[coo.row] * d[coo.col]
    keep = np.abs(coo.data) >= threshold * scale
    Q = sp.csc_matrix((coo.data[keep], (coo.row[keep], coo.col[keep])),
                      shape=(n, n))
    Q.sort_indices()

    def warn(count, min_w, max_w):
        how = ("clipped" if repair == "clip"
               else "replaced by absolute values")
        warnings.warn(
            f"laplace_whiten: {count} of {n} eigenvalues of the negative "
            f"Hessian were {how} (floor {clip:g} times the largest; "
            f"smallest {min_w:.3g}, largest {max_w:.3g}); the whitened "
            "coordinates are only approximately standard normal",
            RuntimeWarning, stacklevel=3)

    if repair == "abs":
        (whitened, L, perm, name, precision, n_clipped, min_w, max_w,
         rank) = _sparse_whiten(model, q_hat, Q, clip, keep_precision, warn)
    else:
        (whitened, L, perm, name, precision, n_clipped, min_w, max_w,
         rank) = _dense_whiten(model, q_hat, Q.toarray(), (Q != 0).toarray(),
                               "abs" if repair == "abs_dense" else "clip",
                               clip, warn)
        if not keep_precision:
            precision = None
    gradients = {"pattern_hessian": 1 + 2 * n,
                 "marginal_mode": marginal_grads,
                 "final_hessian": final_grads}
    gradients["total"] = sum(gradients.values())
    return whitened, LaplaceWhitenReport(
        n=n, n_latent=len(iu), n_theta=len(it), latent_index=iu, q_hat=q_hat,
        theta_hat=theta_hat, theta_start=theta_start, n_clipped=n_clipped,
        min_eigenvalue=min_w, max_eigenvalue=max_w, nnz_L=int(L.nnz),
        ordering=name, perm=perm, L=L, gradients=gradients,
        pathfinder_seconds=pathfinder_seconds, wall_seconds=time.time() - t0,
        outer=outer, precision=precision, rank=rank, n_colors=n_colors,
        hessian=hessian, repair=repair)
