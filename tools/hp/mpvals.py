import math
from mpmath import mp, mpf


class Vec(list):
    pass


class RVec(list):
    pass


class Mat(list):
    pass


class Unsupported(Exception):
    pass


class StanReject(Exception):
    pass


def is_real(x):
    return isinstance(x, mpf) or isinstance(x, float)


def is_scalar(x):
    return not isinstance(x, list)


def copy(x):
    if isinstance(x, list):
        return type(x)(copy(e) for e in x)
    return x


def to_real(x):
    if isinstance(x, list):
        return type(x)(to_real(e) for e in x)
    if isinstance(x, bool):
        return mpf(int(x))
    return mpf(x) if not isinstance(x, mpf) else x


def flat(x):
    if isinstance(x, Mat):
        r = len(x)
        c = len(x[0]) if r else 0
        return [x[i][j] for j in range(c) for i in range(r)]
    if isinstance(x, list):
        out = []
        for e in x:
            out.extend(flat(e) if isinstance(e, list) else [e])
        return out
    return [x]


def map1(f, x):
    if isinstance(x, list):
        return type(x)(map1(f, e) for e in x)
    return f(x)


def map2(f, a, b):
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            raise StanReject("size mismatch")
        return type(a)(map2(f, x, y) for x, y in zip(a, b))
    if isinstance(a, list):
        return type(a)(map2(f, x, b) for x in a)
    if isinstance(b, list):
        return type(b)(map2(f, a, y) for y in b)
    return f(a, b)


def mat_cols(m):
    return len(m[0]) if len(m) else 0


def transpose(x):
    if isinstance(x, Vec):
        return RVec(x)
    if isinstance(x, RVec):
        return Vec(x)
    if isinstance(x, Mat):
        r = len(x)
        c = mat_cols(x)
        return Mat([[x[i][j] for i in range(r)] for j in range(c)]) if c else Mat()
    raise Unsupported("transpose")


def matmul(a, b):
    if is_scalar(a) or is_scalar(b):
        return map2(lambda p, q: p * q, a, b)
    if isinstance(a, RVec) and isinstance(b, Vec):
        return mp.fsum(p * q for p, q in zip(a, b))
    if isinstance(a, Vec) and isinstance(b, RVec):
        return Mat([[p * q for q in b] for p in a])
    if isinstance(a, Mat) and isinstance(b, Vec):
        return Vec([mp.fsum(p * q for p, q in zip(row, b)) for row in a])
    if isinstance(a, RVec) and isinstance(b, Mat):
        c = mat_cols(b)
        return RVec([mp.fsum(a[k] * b[k][j] for k in range(len(a))) for j in range(c)])
    if isinstance(a, Mat) and isinstance(b, Mat):
        c = mat_cols(b)
        n = len(b)
        return Mat([[mp.fsum(row[k] * b[k][j] for k in range(n)) for j in range(c)]
                    for row in a])
    raise Unsupported("matmul %s %s" % (type(a).__name__, type(b).__name__))
