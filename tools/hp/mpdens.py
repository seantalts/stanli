from mpmath import mp, mpf
from mpvals import StanReject, Unsupported, flat, is_scalar


def lg(x):
    return mp.loggamma(x)


def log1m(x):
    return mp.log1p(-x)


def lchoose(n, k):
    return lg(n + 1) - lg(k + 1) - lg(n - k + 1)


def xlogy(x, y):
    if x == 0:
        return mpf(0)
    return x * mp.log(y)


def xlog1my(x, y):
    if x == 0:
        return mpf(0)
    return x * mp.log1p(-y)


def log_inv_logit(a):
    return -mp.log1p(mp.exp(-a)) if a >= 0 else a - mp.log1p(mp.exp(a))


def log1m_inv_logit(a):
    return -mp.log1p(mp.exp(a)) if a <= 0 else -a - mp.log1p(mp.exp(-a))


def nlsp():
    return -mp.log(mp.sqrt(2 * mp.pi))


def _terms():
    T = {}

    def d(name, *terms):
        T[name] = terms

    d("normal", ((), lambda y, m, s: nlsp()), ((2,), lambda y, m, s: -mp.log(s)),
      ((0, 1, 2), lambda y, m, s: -((y - m) / s) ** 2 / 2))
    d("std_normal", ((), lambda y: nlsp()), ((0,), lambda y: -y * y / 2))
    d("lognormal", ((2,), lambda y, m, s: -mp.log(s)),
      ((0,), lambda y, m, s: -mp.log(y)),
      ((0, 1, 2), lambda y, m, s: nlsp() - ((mp.log(y) - m) / s) ** 2 / 2))
    d("student_t", ((1,), lambda y, n, m, s: lg((n + 1) / 2) - lg(n / 2) - mp.log(n) / 2),
      ((), lambda y, n, m, s: -mp.log(mp.pi) / 2), ((3,), lambda y, n, m, s: -mp.log(s)),
      ((0, 1, 2, 3), lambda y, n, m, s: -(n / 2 + mpf(1) / 2) * mp.log1p(((y - m) / s) ** 2 / n)))
    d("cauchy", ((), lambda y, m, s: -mp.log(mp.pi)), ((2,), lambda y, m, s: -mp.log(s)),
      ((0, 1, 2), lambda y, m, s: -mp.log1p(((y - m) / s) ** 2)))
    d("exponential", ((1,), lambda y, b: mp.log(b)), ((0, 1), lambda y, b: -b * y))
    d("beta", ((1,), lambda y, a, b: -lg(a)), ((2,), lambda y, a, b: -lg(b)),
      ((1, 2), lambda y, a, b: lg(a + b)),
      ((0, 1), lambda y, a, b: (a - 1) * mp.log(y)),
      ((0, 2), lambda y, a, b: (b - 1) * log1m(y)))
    d("gamma", ((1,), lambda y, a, b: -lg(a)), ((1, 2), lambda y, a, b: a * mp.log(b)),
      ((0, 1), lambda y, a, b: (a - 1) * mp.log(y)), ((0, 2), lambda y, a, b: -b * y))
    d("inv_gamma", ((1,), lambda y, a, b: -lg(a)), ((1, 2), lambda y, a, b: a * mp.log(b)),
      ((0, 1), lambda y, a, b: -(a + 1) * mp.log(y)), ((0, 2), lambda y, a, b: -b / y))
    d("binomial", ((), lambda n, N, t: lchoose(N, n)),
      ((2,), lambda n, N, t: xlogy(n, t) + xlog1my(N - n, t)))
    d("binomial_logit", ((), lambda n, N, a: lchoose(N, n)),
      ((2,), lambda n, N, a: n * log_inv_logit(a) + (N - n) * log1m_inv_logit(a)))
    d("bernoulli", ((1,), lambda n, t: mp.log(t) if n else log1m(t)))
    d("bernoulli_logit", ((1,), lambda n, a: log_inv_logit(a) if n else log1m_inv_logit(a)))
    d("poisson", ((), lambda n, l: -lg(n + 1)), ((1,), lambda n, l: xlogy(n, l) - l))
    d("poisson_log", ((), lambda n, a: -lg(n + 1)), ((1,), lambda n, a: n * a - mp.exp(a)))
    d("uniform", ((0, 1, 2), lambda y, a, b: mpf(0) if a <= y <= b else mpf("-inf")),
      ((1, 2), lambda y, a, b: -mp.log(b - a)))
    d("double_exponential", ((), lambda y, m, s: -mp.log(2)), ((2,), lambda y, m, s: -mp.log(s)),
      ((0, 1, 2), lambda y, m, s: -abs(y - m) / s))
    d("logistic", ((2,), lambda y, m, s: -mp.log(s)),
      ((0, 1, 2), lambda y, m, s: -(y - m) / s - 2 * mp.log1p(mp.exp(-(y - m) / s))))
    d("weibull", ((1,), lambda y, a, s: mp.log(a)), ((1, 2), lambda y, a, s: -a * mp.log(s)),
      ((0, 1), lambda y, a, s: (a - 1) * mp.log(y)), ((0, 1, 2), lambda y, a, s: -(y / s) ** a))
    d("chi_square", ((1,), lambda y, n: -(n / 2) * mp.log(2) - lg(n / 2)),
      ((0, 1), lambda y, n: (n / 2 - 1) * mp.log(y)), ((0,), lambda y, n: -y / 2))
    d("inv_chi_square", ((1,), lambda y, n: -(n / 2) * mp.log(2) - lg(n / 2)),
      ((0, 1), lambda y, n: -(n / 2 + 1) * mp.log(y)), ((0,), lambda y, n: -1 / (2 * y)))
    d("neg_binomial_2", ((1,), lambda n, m, p: lchoose(n + p - 1, n)),
      ((1, 2), lambda n, m, p: xlogy(n, m) - n * mp.log(m + p) + p * (mp.log(p) - mp.log(m + p))))
    d("neg_binomial_2_log",
      ((2,), lambda n, e, p: lchoose(n + p - 1, n)),
      ((1, 2), lambda n, e, p: n * e - n * mp.log(mp.exp(e) + p)
                               + p * (mp.log(p) - mp.log(mp.exp(e) + p))))
    return T


TERMS = _terms()


def arity(name):
    return len(TERMS[name][0][1].__code__.co_varnames) if name in TERMS else None


def lpdf(name, args, ads, propto):
    if name not in TERMS:
        raise Unsupported("density " + name)
    terms = TERMS[name]
    n = None
    cols = []
    for a in args:
        if isinstance(a, list):
            f = flat(a)
            cols.append(f)
            if n is None or len(f) > n:
                n = len(f)
            if len(f) == 0:
                return mpf(0)
        else:
            cols.append(None)
    if n is None:
        n = 1
    total = []
    for deps, fn in terms:
        if propto and not any(ads[i] for i in deps):
            continue
        for i in range(n):
            vals = [args[k] if c is None else (c[i] if len(c) == n else c[0])
                    for k, c in enumerate(cols)]
            total.append(fn(*vals))
    return mp.fsum(total) if total else mpf(0)
