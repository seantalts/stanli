from mpmath import mp, mpf
from mpvals import Mat, RVec, StanReject, Unsupported, Vec
from mpdens import log_inv_logit, log1m_inv_logit


def elementwise(kind, bargs):
    name = kind
    if name == "Identity":
        return lambda u, b: (u, mpf(0))
    if name == "Lower":
        return lambda u, b: (mp.exp(u) + b[0], u)
    if name == "Upper":
        return lambda u, b: (b[0] - mp.exp(u), u)
    if name == "LowerUpper":
        def f(u, b):
            lo, hi = b
            return (lo + (hi - lo) * (1 / (1 + mp.exp(-u))),
                    mp.log(hi - lo) + log_inv_logit(u) + log1m_inv_logit(u))
        return f
    if name == "Offset":
        return lambda u, b: (u + b[0], mpf(0))
    if name == "Multiplier":
        return lambda u, b: (u * b[0], mp.log(b[0]))
    if name == "OffsetMultiplier":
        return lambda u, b: (b[0] + b[1] * u, mp.log(b[1]))
    return None


def ordered(u):
    x = [u[0]]
    for k in range(1, len(u)):
        x.append(x[-1] + mp.exp(u[k]))
    return x, mp.fsum(u[1:])


def positive_ordered(u):
    x = [mp.exp(u[0])]
    for k in range(1, len(u)):
        x.append(x[-1] + mp.exp(u[k]))
    return x, mp.fsum(u)


def simplex(y):
    N = len(y)
    z = [mpf(0)] * (N + 1)
    if N == 0:
        return [mpf(1)], mpf(0)
    sum_w = mpf(0)
    for i in range(N, 0, -1):
        n = mpf(i)
        w = y[i - 1] / mp.sqrt(n * (n + 1))
        sum_w += w
        z[i - 1] += sum_w
        z[i] -= w * n
    mx = max(z)
    d = mp.fsum(mp.exp(e - mx) for e in z)
    x = [mp.exp(e - mx) / d for e in z]
    return x, -(N + 1) * (mx + mp.log(d)) + mp.log(N + 1) / 2


def sum_to_zero(y):
    N = len(y)
    z = [mpf(0)] * (N + 1)
    if N == 0:
        return z, mpf(0)
    sum_w = mpf(0)
    for i in range(N, 0, -1):
        n = mpf(i)
        w = y[i - 1] / mp.sqrt(n * (n + 1))
        sum_w += w
        z[i - 1] += sum_w
        z[i] -= w * n
    return z, mpf(0)


def corr_constrain(u):
    x = mp.tanh(u)
    return x, mp.log1p(-x * x)


def cholesky_corr(u, K):
    z = [corr_constrain(e) for e in u]
    lp = mp.fsum(j for _, j in z)
    zv = [a for a, _ in z]
    x = [[mpf(0)] * K for _ in range(K)]
    if K == 0:
        return Mat(x), lp
    x[0][0] = mpf(1)
    k = 0
    for i in range(1, K):
        x[i][0] = zv[k]
        k += 1
        sum_sqs = x[i][0] ** 2
        for j in range(1, i):
            lp += mp.log1p(-sum_sqs) / 2
            x[i][j] = zv[k] * mp.sqrt(1 - sum_sqs)
            k += 1
            sum_sqs += x[i][j] ** 2
        x[i][i] = mp.sqrt(1 - sum_sqs)
    return Mat(x), lp


def cholesky_cov(u, M, N):
    pos = 0
    L = [[mpf(0)] * N for _ in range(M)]
    for m in range(N):
        for n in range(m):
            L[m][n] = u[pos]
            pos += 1
        L[m][m] = mp.exp(u[pos])
        pos += 1
    for m in range(N, M):
        for n in range(N):
            L[m][n] = u[pos]
            pos += 1
    lp = mp.fsum(u[(m * (m + 1)) // 2 + m] for m in range(N))
    return Mat(L), lp
