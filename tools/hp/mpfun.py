from mpmath import mp, mpf
from mpvals import (Mat, RVec, StanReject, Unsupported, Vec, copy, flat, is_scalar, map1, map2,
                    mat_cols, matmul, to_real, transpose)
from mpdens import lg, log1m, log_inv_logit, log1m_inv_logit


def ew(f):
    return lambda x: map1(f, x)


def inv_logit(a):
    return 1 / (1 + mp.exp(-a)) if a >= 0 else mp.exp(a) / (1 + mp.exp(a))


def Phi(x):
    return mp.erfc(-x / mp.sqrt(2)) / 2


def lse(xs):
    xs = flat(xs)
    if not xs:
        return mpf("-inf")
    m = max(xs)
    return m + mp.log(mp.fsum(mp.exp(x - m) for x in xs))


def mean(x):
    f = flat(x)
    return mp.fsum(f) / len(f)


def variance(x):
    f = flat(x)
    m = mean(f)
    return mp.fsum((e - m) ** 2 for e in f) / (len(f) - 1)


def tdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def divide(a, b):
    if isinstance(a, int) and isinstance(b, int) and not isinstance(a, bool):
        return tdiv(a, b)
    if isinstance(a, list) and not isinstance(b, list):
        return map1(lambda e: e / b, a)
    return map2(lambda p, q: p / q, a, b)


def plus(a, b):
    return map2(lambda p, q: p + q, a, b)


def minus(a, b):
    return map2(lambda p, q: p - q, a, b)


def cmp(f):
    return lambda a, b: int(f(a, b))


def rep_vector(x, n):
    return Vec([x] * n)


def rep_array(x, *ns):
    def go(k):
        if k == len(ns):
            return copy(x)
        return [go(k + 1) for _ in range(ns[k])]
    return go(0)


def rep_matrix(x, r, c=None):
    if c is None:
        if isinstance(x, Vec):
            return Mat([[e] * r for e in x])
        return Mat([list(x) for _ in range(r)])
    return Mat([[x] * c for _ in range(r)])


def to_vector(x):
    return Vec(flat(x))


def to_row_vector(x):
    return RVec(flat(x))


def to_matrix(x, r=None, c=None, *rest):
    if r is None:
        if isinstance(x, Mat):
            return x
        if isinstance(x, Vec):
            return Mat([[e] for e in x])
        if isinstance(x, RVec):
            return Mat([list(x)])
        return Mat([list(row) for row in x])
    f = flat(x)
    return Mat([[f[j * r + i] for j in range(c)] for i in range(r)])


def rows(x):
    if isinstance(x, Mat):
        return len(x)
    if isinstance(x, RVec):
        return 1
    return len(x)


def cols(x):
    if isinstance(x, Mat):
        return mat_cols(x)
    if isinstance(x, RVec):
        return len(x)
    return 1


def sumf(x):
    f = flat(x)
    if f and isinstance(f[0], int):
        return sum(f)
    return mp.fsum(f)


def dot(a, b):
    return mp.fsum(p * q for p, q in zip(flat(a), flat(b)))


def cumsum(x):
    out, s = [], 0
    for e in flat(x):
        s = s + e
        out.append(s)
    return type(x)(out)


def append_row(a, b):
    if isinstance(a, Mat) or isinstance(b, Mat):
        a = a if isinstance(a, Mat) else Mat([list(a)])
        b = b if isinstance(b, Mat) else Mat([list(b)])
        return Mat(list(a) + list(b))
    a = list(a) if isinstance(a, list) else [a]
    b = list(b) if isinstance(b, list) else [b]
    return Vec(a + b)


def append_col(a, b):
    if isinstance(a, Mat) or isinstance(b, Mat):
        a = a if isinstance(a, Mat) else Mat([[e] for e in a])
        b = b if isinstance(b, Mat) else Mat([[e] for e in b])
        return Mat([list(x) + list(y) for x, y in zip(a, b)])
    a = list(a) if isinstance(a, list) else [a]
    b = list(b) if isinstance(b, list) else [b]
    return RVec(a + b)


def diag_matrix(v):
    n = len(v)
    return Mat([[v[i] if i == j else mpf(0) for j in range(n)] for i in range(n)])


def fabs(x):
    return abs(x)


def lchoosef(n, k):
    return lg(n + 1) - lg(k + 1) - lg(n - k + 1)


def pow_(a, b):
    return map2(lambda p, q: mp.power(p, q), a, b)


def _inv_Phi(p):
    return mp.sqrt(2) * mp.erfinv(2 * p - 1)


def fmin_(a, b):
    return min(a, b)


def fmax_(a, b):
    return max(a, b)


def softmax(x):
    f = flat(x)
    m = max(f)
    e = [mp.exp(v - m) for v in f]
    s = mp.fsum(e)
    return Vec([v / s for v in e])


def log_softmax(x):
    f = flat(x)
    l = lse(f)
    return Vec([v - l for v in f])


def sd(x):
    return mp.sqrt(variance(x))


def size_(x):
    return len(x) if isinstance(x, list) else 1


def num_elements(x):
    return len(flat(x))


def inc_beta(a, b, x):
    return mp.betainc(a, b, 0, x, regularized=True)


def sqrt_(x):
    return mp.sqrt(x)


def mkF():
    F = {}
    F.update({
        "exp": ew(mp.exp), "log": ew(mp.log), "log1p": ew(mp.log1p), "log1m": ew(log1m),
        "expm1": ew(mp.expm1), "sqrt": ew(mp.sqrt), "square": ew(lambda x: x * x),
        "inv": ew(lambda x: 1 / x), "inv_sqrt": ew(lambda x: 1 / mp.sqrt(x)),
        "inv_square": ew(lambda x: 1 / (x * x)), "cbrt": ew(mp.cbrt),
        "sin": ew(mp.sin), "cos": ew(mp.cos), "tan": ew(mp.tan), "tanh": ew(mp.tanh),
        "sinh": ew(mp.sinh), "cosh": ew(mp.cosh), "asin": ew(mp.asin), "acos": ew(mp.acos),
        "atan": ew(mp.atan), "asinh": ew(mp.asinh), "acosh": ew(mp.acosh),
        "atanh": ew(mp.atanh), "lgamma": ew(lg), "tgamma": ew(mp.gamma),
        "digamma": ew(mp.digamma), "erf": ew(mp.erf), "erfc": ew(mp.erfc),
        "inv_logit": ew(inv_logit), "logit": ew(lambda x: mp.log(x / (1 - x))),
        "log_inv_logit": ew(log_inv_logit), "log1m_inv_logit": ew(log1m_inv_logit),
        "inv_cloglog": ew(lambda x: 1 - mp.exp(-mp.exp(x))),
        "Phi": ew(Phi), "Phi_approx": ew(lambda x: inv_logit(mpf("0.07056") * x ** 3 + mpf("1.5976") * x)),
        "inv_Phi": ew(_inv_Phi),
        "log1p_exp": ew(lambda x: mp.log1p(mp.exp(x)) if x < 0 else x + mp.log1p(mp.exp(-x))),
        "log_sum_exp": lambda *a: lse(a[0]) if len(a) == 1 else lse([a[0], a[1]]),
        "fabs": ew(abs), "abs": ew(abs), "floor": ew(mp.floor), "ceil": ew(mp.ceil),
        "round": ew(lambda x: mp.floor(x + mpf(1) / 2)), "trunc": ew(mp.floor),
        "pow": pow_, "fmin": fmin_, "fmax": fmax_, "atan2": lambda a, b: mp.atan2(a, b),
        "hypot": lambda a, b: mp.hypot(a, b), "log_diff_exp": lambda a, b: a + mp.log1p(-mp.exp(b - a)),
        "lbeta": lambda a, b: lg(a) + lg(b) - lg(a + b),
        "lchoose": lchoosef, "binomial_coefficient_log": lchoosef,
        "lmgamma": lambda k, x: k * (k - 1) / mpf(4) * mp.log(mp.pi) + mp.fsum(lg(x + (1 - j) / mpf(2)) for j in range(1, k + 1)),
        "inc_beta": inc_beta,
        "sum": sumf, "mean": mean, "variance": variance, "sd": sd,
        "min": lambda *a: (min(flat(a[0])) if len(a) == 1 else min(a)),
        "max": lambda *a: (max(flat(a[0])) if len(a) == 1 else max(a)),
        "dot_product": dot, "dot_self": lambda x: dot(x, x),
        "rows": rows, "cols": cols, "size": size_, "num_elements": num_elements,
        "rep_vector": rep_vector, "rep_row_vector": lambda x, n: RVec([x] * n),
        "rep_array": rep_array, "rep_matrix": rep_matrix,
        "to_vector": to_vector, "to_row_vector": to_row_vector, "to_matrix": to_matrix,
        "to_array_1d": lambda x: flat(x), "cumulative_sum": cumsum,
        "append_row": append_row, "append_col": append_col, "diag_matrix": diag_matrix,
        "softmax": softmax, "log_softmax": log_softmax,
        "log_mix": None, "prod": lambda x: mp.fprod(flat(x)),
        "is_inf": lambda x: int(mp.isinf(x)), "is_nan": lambda x: int(mp.isnan(x)),
        "int_step": lambda x: int(x > 0), "step": lambda x: mpf(int(x >= 0)),
        "sign": ew(lambda x: (x > 0) - (x < 0)),
        "multiply_log": lambda a, b: mpf(0) if a == 0 and b == 0 else a * mp.log(b),
        "lmultiply": lambda a, b: mpf(0) if a == 0 and b == 0 else a * mp.log(b),
        "logical_negation": None,
        "tcrossprod": lambda m: matmul(m, transpose(m)),
        "crossprod": lambda m: matmul(transpose(m), m),
        "transpose": transpose,
        "diagonal": lambda m: Vec([m[i][i] for i in range(len(m))]),
        "head": lambda x, n: type(x)(list(x)[:n]),
        "tail": lambda x, n: type(x)(list(x)[len(x) - n:]),
        "segment": lambda x, i, n: type(x)(list(x)[i - 1:i - 1 + n]),
        "reverse": lambda x: type(x)(list(x)[::-1]),
        "identity_matrix": lambda n: Mat([[mpf(int(i == j)) for j in range(n)] for i in range(n)]),
        "norm2": lambda x: mp.sqrt(dot(x, x)),
        "squared_distance": lambda a, b: mp.fsum((p - q) ** 2 for p, q in zip(flat(a), flat(b))),
        "distance": lambda a, b: mp.sqrt(mp.fsum((p - q) ** 2 for p, q in zip(flat(a), flat(b)))),
        "logical_or": None,
    })
    return {k: v for k, v in F.items() if v is not None}


F = mkF()
