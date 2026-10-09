from mpmath import mp, mpf, matrix
import mpdens
from mpdens import lg, lchoose, log1m, log_inv_logit, log1m_inv_logit
from mpfun import F, Phi, inv_logit, lse
from mpvals import Mat, RVec, Unsupported, Vec, flat, map1, mat_cols, matmul, transpose

EXTRA_DENS = {}
CDFS = {}


def inc(propto, flags, deps):
    return (not propto) or any(flags[i] for i in deps)


def rows_of(x):
    if isinstance(x, Mat):
        return [list(r) for r in x]
    if isinstance(x, list) and x and isinstance(x[0], list):
        return [list(r) for r in x]
    return [list(x)]


def bcast(args):
    cols = [flat(a) if isinstance(a, list) else None for a in args]
    n = max([len(c) for c in cols if c is not None] + [1])
    return cols, n


def pick(cols, args, i, n):
    return [args[k] if c is None else (c[i] if len(c) == n else c[0]) for k, c in enumerate(cols)]


def vec_density(deps_terms):
    def g(args, flags, propto):
        cols, n = bcast(args)
        if any(c is not None and len(c) == 0 for c in cols):
            return mpf(0)
        tot = []
        for deps, fn in deps_terms:
            if inc(propto, flags, deps):
                for i in range(n):
                    tot.append(fn(*pick(cols, args, i, n)))
        return mp.fsum(tot) if tot else mpf(0)
    return g


def xlogy(x, y):
    return mpf(0) if x == 0 else x * mp.log(y)


def dirichlet(args, flags, propto):
    th, al = args
    ths = rows_of(th)
    als = rows_of(al)
    n = max(len(ths), len(als))
    tot = []
    for i in range(n):
        t = ths[i if len(ths) > 1 else 0]
        a = als[i if len(als) > 1 else 0]
        if inc(propto, flags, (1,)):
            tot.append(lg(mp.fsum(a)) - mp.fsum(lg(x) for x in a))
        if inc(propto, flags, (0, 1)):
            tot.append(mp.fsum((x - 1) * mp.log(y) for x, y in zip(a, t)))
    return mp.fsum(tot) if tot else mpf(0)


def to_mp(m):
    return matrix([list(r) for r in m])


def logdet_chol(S):
    L = mp.cholesky(to_mp(S))
    return 2 * mp.fsum(mp.log(L[i, i]) for i in range(L.rows)), L


def multi_normal(args, flags, propto):
    y, mu, S = args
    ys, mus = rows_of(y), rows_of(mu)
    n = max(len(ys), len(mus))
    K = len(S)
    tot = []
    if inc(propto, flags, ()):
        tot.append(mpdens.nlsp() * K * n)
    if inc(propto, flags, (0, 1, 2)):
        ld, L = logdet_chol(S)
        for i in range(n):
            d = matrix([a - b for a, b in zip(ys[i if len(ys) > 1 else 0], mus[i if len(mus) > 1 else 0])])
            z = mp.cholesky_solve(to_mp(S), d)
            tot.append(-mp.fsum(d[j] * z[j] for j in range(K)) / 2)
        if inc(propto, flags, (2,)):
            tot.append(-ld / 2 * n)
    return mp.fsum(tot) if tot else mpf(0)


def multi_normal_cholesky(args, flags, propto):
    y, mu, L = args
    ys, mus = rows_of(y), rows_of(mu)
    n = max(len(ys), len(mus))
    K = len(L)
    tot = []
    if inc(propto, flags, ()):
        tot.append(mpdens.nlsp() * K * n)
    if inc(propto, flags, (0, 1, 2)):
        Lm = to_mp(L)
        for i in range(n):
            d = matrix([a - b for a, b in zip(ys[i if len(ys) > 1 else 0], mus[i if len(mus) > 1 else 0])])
            z = mp.lu_solve(Lm, d)
            tot.append(-mp.fsum(z[j] ** 2 for j in range(K)) / 2)
        if inc(propto, flags, (2,)):
            tot.append(-mp.fsum(mp.log(L[j][j]) for j in range(K)) * n)
    return mp.fsum(tot) if tot else mpf(0)


def lin_pred(x, alpha, beta):
    if isinstance(x, RVec):
        x = Mat([list(x)])
    eta = matmul(x, beta)
    eta = [eta] if not isinstance(eta, list) else list(eta)
    a = flat(alpha) if isinstance(alpha, list) else [alpha]
    return [e + (a[i] if len(a) > 1 else a[0]) for i, e in enumerate(eta)]


def glm_n(y, eta):
    return max(len(flat(y)) if isinstance(y, list) else 1, len(eta))


def bernoulli_logit_glm(args, flags, propto):
    y, x, a, b = args
    if not inc(propto, flags, (1, 2, 3)):
        return mpf(0)
    ys = flat(y) if isinstance(y, list) else [y]
    eta = lin_pred(x, a, b)
    n = max(len(ys), len(eta))
    return mp.fsum((log_inv_logit(eta[i if len(eta) > 1 else 0]) if ys[i if len(ys) > 1 else 0]
                    else log1m_inv_logit(eta[i if len(eta) > 1 else 0])) for i in range(n))


def poisson_log_glm(args, flags, propto):
    y, x, a, b = args
    if not inc(propto, flags, (1, 2, 3)):
        return mpf(0)
    ys = flat(y) if isinstance(y, list) else [y]
    eta = lin_pred(x, a, b)
    n = max(len(ys), len(eta))
    tot = [(-lg(ys[i if len(ys) > 1 else 0] + 1) if inc(propto, flags, ()) else 0)
           + ys[i if len(ys) > 1 else 0] * eta[i if len(eta) > 1 else 0] - mp.exp(eta[i if len(eta) > 1 else 0])
           for i in range(n)]
    return mp.fsum(tot)


def neg_binomial_2_log_glm(args, flags, propto):
    y, x, a, b, phi = args
    if not inc(propto, flags, (1, 2, 3, 4)):
        return mpf(0)
    ys = flat(y) if isinstance(y, list) else [y]
    eta = lin_pred(x, a, b)
    ph = flat(phi) if isinstance(phi, list) else [phi]
    n = max(len(ys), len(eta), len(ph))
    tot = []
    for i in range(n):
        yi = ys[i if len(ys) > 1 else 0]
        th = eta[i if len(eta) > 1 else 0]
        p = ph[i if len(ph) > 1 else 0]
        if inc(propto, flags, ()):
            tot.append(-lg(yi + 1))
        if inc(propto, flags, (4,)):
            tot.append(xlogy(p, p) - lg(p) + lg(yi + p))
        tot.append(-(yi + p) * lse([th, mp.log(p)]))
        if inc(propto, flags, (1, 2, 3)):
            tot.append(yi * th)
    return mp.fsum(tot)


def normal_id_glm(args, flags, propto):
    y, x, a, b, s = args
    ys = flat(y) if isinstance(y, list) else [y]
    eta = lin_pred(x, a, b)
    ss = flat(s) if isinstance(s, list) else [s]
    n = max(len(ys), len(eta), len(ss))
    tot = []
    for i in range(n):
        yi = ys[i if len(ys) > 1 else 0]
        mu = eta[i if len(eta) > 1 else 0]
        sg = ss[i if len(ss) > 1 else 0]
        if inc(propto, flags, ()):
            tot.append(mpdens.nlsp())
        if inc(propto, flags, (4,)):
            tot.append(-mp.log(sg))
        tot.append(-((yi - mu) / sg) ** 2 / 2)
    return mp.fsum(tot)


def categorical(args, flags, propto):
    y, th = args
    if not inc(propto, flags, (1,)):
        return mpf(0)
    ys = flat(y) if isinstance(y, list) else [y]
    return mp.fsum(mp.log(th[k - 1]) for k in ys)


def categorical_logit(args, flags, propto):
    y, be = args
    if not inc(propto, flags, (1,)):
        return mpf(0)
    ys = flat(y) if isinstance(y, list) else [y]
    l = lse(be)
    return mp.fsum(be[k - 1] - l for k in ys)


def beta_binomial(args, flags, propto):
    if not inc(propto, flags, (2, 3)):
        return mpf(0)
    cols, n = bcast(args)
    tot = []
    for i in range(n):
        k, N, a, b = pick(cols, args, i, n)
        if k < 0 or k > N:
            return mpf("-inf")
        if inc(propto, flags, ()):
            tot.append(lchoose(N, k))
        tot.append(lg(k + a) + lg(N - k + b) - lg(N + a + b) - (lg(a) + lg(b) - lg(a + b)))
    return mp.fsum(tot)


def ordered_logistic(args, flags, propto):
    k, eta, c = args
    if not inc(propto, flags, (1, 2)):
        return mpf(0)
    ks = flat(k) if isinstance(k, list) else [k]
    es = flat(eta) if isinstance(eta, list) else [eta]
    n = max(len(ks), len(es))
    cs = c if (c and isinstance(c[0], list)) else None
    tot = []
    for i in range(n):
        ki = ks[i if len(ks) > 1 else 0]
        e = es[i if len(es) > 1 else 0]
        cc = cs[i if len(cs) > 1 else 0] if cs else c
        K = len(cc) + 1
        if ki == 1:
            tot.append(log1m_inv_logit(e - cc[0]) if False else log_inv_logit(cc[0] - e))
        elif ki == K:
            tot.append(log_inv_logit(e - cc[K - 2]))
        else:
            tot.append(mp.log(inv_logit(e - cc[ki - 2]) - inv_logit(e - cc[ki - 1])))
    return mp.fsum(tot)


def ordered_probit(args, flags, propto):
    k, eta, c = args
    if not inc(propto, flags, (1, 2)):
        return mpf(0)
    ks = flat(k) if isinstance(k, list) else [k]
    es = flat(eta) if isinstance(eta, list) else [eta]
    n = max(len(ks), len(es))
    tot = []
    for i in range(n):
        ki = ks[i if len(ks) > 1 else 0]
        e = es[i if len(es) > 1 else 0]
        K = len(c) + 1
        if ki == 1:
            tot.append(mp.log(Phi(c[0] - e)))
        elif ki == K:
            tot.append(mp.log(Phi(e - c[K - 2])))
        else:
            tot.append(mp.log(Phi(c[ki - 1] - e) - Phi(c[ki - 2] - e)))
    return mp.fsum(tot)


EXTRA_DENS.update({
    "dirichlet": dirichlet, "multi_normal": multi_normal,
    "multi_normal_cholesky": multi_normal_cholesky,
    "bernoulli_logit_glm": bernoulli_logit_glm, "poisson_log_glm": poisson_log_glm,
    "neg_binomial_2_log_glm": neg_binomial_2_log_glm, "normal_id_glm": normal_id_glm,
    "categorical": categorical, "categorical_logit": categorical_logit,
    "beta_binomial": beta_binomial, "ordered_logistic": ordered_logistic,
    "ordered_probit": ordered_probit,
})


def summed(f):
    def g(*args):
        cols, n = bcast(args)
        if any(c is not None and len(c) == 0 for c in cols):
            return mpf(0)
        return mp.fsum(f(*pick(cols, args, i, n)) for i in range(n))
    return g


def t_cdf(z, nu):
    x = nu / (nu + z * z)
    tail = mp.betainc(nu / 2, mpf(1) / 2, 0, x, regularized=True) / 2
    return 1 - tail if z > 0 else tail


def t_ccdf(z, nu):
    return t_cdf(-z, nu)


def reg_lower(a, x):
    return mp.gammainc(a, 0, x, regularized=True)


def reg_upper(a, x):
    return mp.gammainc(a, x, mp.inf, regularized=True)


def std_cdf(z):
    return Phi(z)


CDFS.update({
    "student_t_lcdf": summed(lambda y, nu, m, s: mp.log(t_cdf((y - m) / s, nu))),
    "student_t_lccdf": summed(lambda y, nu, m, s: mp.log(t_ccdf((y - m) / s, nu))),
    "student_t_cdf": summed(lambda y, nu, m, s: t_cdf((y - m) / s, nu)),
    "normal_lcdf": summed(lambda y, m, s: mp.log(Phi((y - m) / s))),
    "normal_lccdf": summed(lambda y, m, s: mp.log(Phi(-(y - m) / s))),
    "normal_cdf": summed(lambda y, m, s: Phi((y - m) / s)),
    "std_normal_lcdf": summed(lambda y: mp.log(Phi(y))),
    "std_normal_lccdf": summed(lambda y: mp.log(Phi(-y))),
    "lognormal_lcdf": summed(lambda y, m, s: mp.log(Phi((mp.log(y) - m) / s)) if y > 0 else mpf("-inf")),
    "lognormal_lccdf": summed(lambda y, m, s: mp.log(Phi(-(mp.log(y) - m) / s)) if y > 0 else mpf(0)),
    "gamma_lcdf": summed(lambda y, a, b: mp.log(reg_lower(a, b * y))),
    "gamma_lccdf": summed(lambda y, a, b: mp.log(reg_upper(a, b * y))),
    "inv_gamma_lcdf": summed(lambda y, a, b: mp.log(reg_upper(a, b / y))),
    "inv_gamma_lccdf": summed(lambda y, a, b: mp.log(reg_lower(a, b / y))),
    "exponential_lcdf": summed(lambda y, b: mp.log(-mp.expm1(-b * y))),
    "exponential_lccdf": summed(lambda y, b: -b * y),
    "weibull_lcdf": summed(lambda y, a, s: mp.log(-mp.expm1(-(y / s) ** a))),
    "weibull_lccdf": summed(lambda y, a, s: -(y / s) ** a),
    "uniform_lcdf": summed(lambda y, a, b: mp.log((min(max(y, a), b) - a) / (b - a))),
    "uniform_lccdf": summed(lambda y, a, b: mp.log((b - min(max(y, a), b)) / (b - a))),
    "cauchy_lcdf": summed(lambda y, m, s: mp.log(mp.atan((y - m) / s) / mp.pi + mpf(1) / 2)),
    "cauchy_lccdf": summed(lambda y, m, s: mp.log(mpf(1) / 2 - mp.atan((y - m) / s) / mp.pi)),
    "logistic_lcdf": summed(lambda y, m, s: log_inv_logit((y - m) / s)),
    "logistic_lccdf": summed(lambda y, m, s: log1m_inv_logit((y - m) / s)),
    "poisson_lcdf": summed(lambda n, l: mp.log(reg_upper(n + 1, l)) if n >= 0 else mpf("-inf")),
    "poisson_lccdf": summed(lambda n, l: mp.log(reg_lower(n + 1, l))),
    "beta_lcdf": summed(lambda y, a, b: mp.log(mp.betainc(a, b, 0, y, regularized=True))),
    "beta_lccdf": summed(lambda y, a, b: mp.log(mp.betainc(b, a, 0, 1 - y, regularized=True))),
    "binomial_lcdf": summed(lambda n, N, t: mp.log(mp.betainc(N - n, n + 1, 0, 1 - t, regularized=True)) if n < N else mpf(0)),
    "binomial_lccdf": summed(lambda n, N, t: mp.log(mp.betainc(n + 1, N - n, 0, t, regularized=True)) if n < N else mpf("-inf")),
    "neg_binomial_2_lcdf": summed(lambda n, m, p: mp.log(mp.betainc(p, n + 1, 0, p / (m + p), regularized=True))),
    "neg_binomial_2_lccdf": summed(lambda n, m, p: mp.log(mp.betainc(n + 1, p, 0, m / (m + p), regularized=True))),
})


def log_mix(*a):
    if len(a) == 3:
        t, l1, l2 = a
        return lse([mp.log(t) + l1, mp.log1p(-t) + l2])
    if len(a) == 2:
        t, l = a
        return lse([mp.log(x) + y for x, y in zip(flat(t), flat(l))])
    raise Unsupported("log_mix arity")


def cholesky_decompose(m):
    L = mp.cholesky(to_mp(m))
    return Mat([[L[i, j] for j in range(L.cols)] for i in range(L.rows)])


def dims(x):
    out = []
    t = x
    while isinstance(t, list):
        if isinstance(t, Mat):
            out += [len(t), mat_cols(t)]
            break
        out.append(len(t))
        if not t:
            break
        t = t[0]
    return out


def choose(n, k):
    from math import comb
    return comb(n, k) if 0 <= k <= n else 0


F.update({
    "negative_infinity": lambda: mpf("-inf"), "positive_infinity": lambda: mpf("inf"),
    "not_a_number": lambda: mpf("nan"), "pi": lambda: +mp.pi, "e": lambda: mp.e,
    "log10": lambda x: map1(mp.log10, x), "log2": lambda x: map1(lambda v: mp.log(v) / mp.log(2), x),
    "log1m_exp": lambda x: map1(lambda v: mp.log(-mp.expm1(v)), x),
    "log_mix": log_mix, "cholesky_decompose": cholesky_decompose, "dims": dims,
    "choose": choose,
    "diag_pre_multiply": lambda v, m: Mat([[v[i] * e for e in row] for i, row in enumerate(m)]),
    "diag_post_multiply": lambda m, v: Mat([[e * v[j] for j, e in enumerate(row)] for row in m]),
    "quad_form_diag": lambda m, v: Mat([[v[i] * m[i][j] * v[j] for j in range(len(v))] for i in range(len(v))]),
    "multiply_lower_tri_self_transpose": lambda m: matmul(Mat([[e if j <= i else mpf(0) for j, e in enumerate(r)] for i, r in enumerate(m)]), transpose(Mat([[e if j <= i else mpf(0) for j, e in enumerate(r)] for i, r in enumerate(m)]))),
    "sub_col": lambda m, i, j, n: Vec([m[i - 1 + k][j - 1] for k in range(n)]),
    "sub_row": lambda m, i, j, n: RVec([m[i - 1][j - 1 + k] for k in range(n)]),
    "col": lambda m, j: Vec([r[j - 1] for r in m]),
    "row": lambda m, i: RVec(list(m[i - 1])),
    "log_inv_logit_diff": lambda a, b: mp.log(inv_logit(a) - inv_logit(b)),
    "logical_negation": lambda x: int(not x),
})


def lkj_const(eta, K):
    Km1 = K - 1
    if eta == 1:
        c = -mp.fsum(lg(mpf(2 * k)) for k in range(1, Km1 // 2 + 1))
        if K % 2 == 1:
            c -= (mpf(K * K - 1) / 4 * mp.log(mp.pi) - mpf(Km1 * Km1) / 4 * mp.log(2)
                  - Km1 * lg(mpf(K + 1) / 2))
        else:
            c -= (mpf(K * (K - 2)) / 4 * mp.log(mp.pi) + mpf(3 * K * K - 4 * K) / 4 * mp.log(2)
                  + K * lg(mpf(K) / 2) - Km1 * lg(mpf(K)))
        return c
    c = Km1 * lg(eta + mpf(Km1) / 2)
    for k in range(1, Km1 + 1):
        c -= mpf(k) / 2 * mp.log(mp.pi) + lg(eta + mpf(Km1 - k) / 2)
    return c


def lkj_corr_cholesky(args, flags, propto):
    L, eta = args
    K = len(L)
    if K == 0:
        return mpf(0)
    tot = []
    if inc(propto, flags, (1,)):
        tot.append(lkj_const(eta, K))
    if inc(propto, flags, (0, 1)):
        Km1 = K - 1
        ld = [mp.log(L[i + 1][i + 1]) for i in range(Km1)]
        vals = [(Km1 - k - 1) * ld[k] + (2 * eta - 2) * ld[k] for k in range(Km1)]
        tot.append(mp.fsum(vals))
    return mp.fsum(tot)


def lkj_corr(args, flags, propto):
    Y, eta = args
    K = len(Y)
    if K == 0:
        return mpf(0)
    tot = []
    if inc(propto, flags, (1,)):
        tot.append(lkj_const(eta, K))
    if inc(propto, flags, (0, 1)):
        ld, _ = logdet_chol(Y)
        tot.append((eta - 1) * ld)
    return mp.fsum(tot)


EXTRA_DENS.update({"lkj_corr_cholesky": lkj_corr_cholesky, "lkj_corr": lkj_corr})

EXTRA_DENS["gumbel"] = vec_density([
    ((0, 1, 2), lambda y, m, b: -((y - m) / b) - mp.exp(-((y - m) / b))),
    ((2,), lambda y, m, b: -mp.log(b))])
EXTRA_DENS["frechet"] = vec_density([
    ((0, 1, 2), lambda y, a, s: -(s / y) ** a),
    ((1,), lambda y, a, s: mp.log(a)),
    ((0, 1), lambda y, a, s: -(a + 1) * mp.log(y)),
    ((1, 2), lambda y, a, s: a * mp.log(s))])


def sqdist(a, b):
    a = flat(a) if isinstance(a, list) else [a]
    b = flat(b) if isinstance(b, list) else [b]
    return mp.fsum((p - q) ** 2 for p, q in zip(a, b))


def gp_cov(kern):
    def f(x, *rest):
        sg, ell = rest
        n = len(x)
        return Mat([[sg ** 2 if i == j else kern(sg, ell, mp.sqrt(sqdist(x[i], x[j]))) for j in range(n)] for i in range(n)])
    return f


def gp_cov_cross(kern):
    def f(*a):
        if len(a) == 3:
            return gp_cov(kern)(*a)
        x1, x2, sg, ell = a
        return Mat([[kern(sg, ell, mp.sqrt(sqdist(p, q))) for q in x2] for p in x1])
    return f


def multinomial(args, flags, propto):
    ns, th = args
    ns = flat(ns)
    th = flat(th)
    tot = []
    if inc(propto, flags, ()):
        tot.append(lg(mpf(sum(ns) + 1)) * 0 + lg(mpf(sum(ns)) + 1) - mp.fsum(lg(mpf(n) + 1) for n in ns))
    if inc(propto, flags, (1,)):
        tot.append(mp.fsum(xlogy(mpf(n), t) for n, t in zip(ns, th)))
    return mp.fsum(tot)


EXTRA_DENS["multinomial"] = multinomial


def add_diag(m, d):
    n = len(m)
    dd = flat(d) if isinstance(d, list) else [d] * n
    return Mat([[e + (dd[i] if i == j else 0) for j, e in enumerate(r)] for i, r in enumerate(m)])


F.update({
    "fma": lambda a, b, c: a * b + c, "add_diag": add_diag,
    "gp_exp_quad_cov": gp_cov_cross(lambda s, l, d: s ** 2 * mp.exp(-d * d / (2 * l * l))),
    "gp_matern32_cov": gp_cov_cross(lambda s, l, d: s ** 2 * (1 + mp.sqrt(3) * d / l) * mp.exp(-mp.sqrt(3) * d / l)),
    "gp_matern52_cov": gp_cov_cross(lambda s, l, d: s ** 2 * (1 + mp.sqrt(5) * d / l + 5 * d * d / (3 * l * l)) * mp.exp(-mp.sqrt(5) * d / l)),
    "gp_exponential_cov": gp_cov_cross(lambda s, l, d: s ** 2 * mp.exp(-d / l)),
    "append_array": lambda a, b: list(a) + list(b),
    "sqrt2": lambda: mp.sqrt(2),
})
