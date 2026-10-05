import sys, json, subprocess, statistics, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import hp_models as H
from hp_models import mp, mpf, log, exp, lgamma, uerr, R, eval_point

def dat(m): return json.load(open(f'{R}/tests/rethinking/{m}.json'))

def run(binary, model, pt):
    out = subprocess.run([f'{R}/{binary}/stanli_check', f'{R}/tests/rethinking/{model}.stan',
                          f'{R}/tests/rethinking/{model}.json', '--point', str(pt)],
                         capture_output=True, text=True, cwd=R).stdout
    f = [l for l in out.splitlines() if l.startswith('OK')][-1].split()
    return [float(x) for x in f[1:]]

def m15_8(const_poisson=True):
    d = dat('ch15_m15_8'); notes, cat, RC = d['notes'], d['cat'], d['RC']
    def lp(u):
        a, b, ku = u
        k = 1 / (1 + exp(-ku))
        t = log(k * (1 - k))
        t += (2 - 1) * log(k) + (2 - 1) * log(1 - k)
        t += -(b / mpf('0.5')) ** 2 / 2
        t += -a * a / 2
        for i in range(100):
            if RC[i] == 0:
                t += cat[i] * log(k) + (1 - cat[i]) * log(1 - k)
                lam = exp(a + b * cat[i])
                t += notes[i] * log(lam) - lam
            else:
                n = notes[i]
                def pl(l): return n * log(l) - l - lgamma(n + 1)
                x = log(k) + pl(exp(a + b)); y = log(1 - k) + pl(exp(a))
                t += log(exp(x) + exp(y))
        return t
    return lp, 3

def gram_model(m, kind):
    d = dat(m)
    N = 151
    B = [mpf(x) for x in d['B']]; G = [mpf(x) for x in d['G']]; M = [mpf(x) for x in d['M']]
    one = [mpf(1)] * N
    return d, N, B, G, M, one

def chol(A, N):
    L = [[mpf(0)] * N for _ in range(N)]
    for i in range(N):
        Li = L[i]
        for j in range(i + 1):
            Lj = L[j]
            s = A[i][j]
            for k in range(j): s -= Li[k] * Lj[k]
            if i == j: Li[j] = mp.sqrt(s)
            else: Li[j] = s / Lj[j]
    return L

def solve_chol(L, N, b):
    y = [None] * N
    for i in range(N):
        s = b[i]
        for k in range(i): s -= L[i][k] * y[k]
        y[i] = s / L[i][i]
    x = [None] * N
    for i in reversed(range(N)):
        s = y[i]
        for k in range(i + 1, N): s -= L[k][i] * x[k]
        x[i] = s / L[i][i]
    return x

def inv_chol(L, N):
    cols = []
    for j in range(N):
        e = [mpf(0)] * N; e[j] = mpf(1)
        cols.append(solve_chol(L, N, e))
    return cols  # symmetric

def m14_10_ref(u, dps):
    mp.dps = dps
    d, N, B, G, M, one = gram_model('ch14_m14_10', 0)
    Rm = [[mpf(x) for x in row] for row in d['R']]
    L = chol(Rm, N)
    logdetR = 2 * sum(log(L[i][i]) for i in range(N))
    Rinv = inv_chol(L, N)
    def dotinv(x, y):
        return sum(x[i] * sum(Rinv[i][j] * y[j] for j in range(N)) for i in range(N))
    vecs = {'1': one, 'M': M, 'G': G, 'B': B}
    names = list(vecs)
    Rv = {k: [sum(Rinv[i][j] * vecs[k][j] for j in range(N)) for i in range(N)] for k in names}
    Q = {(p, q): sum(vecs[p][i] * Rv[q][i] for i in range(N)) for p in names for q in names}
    def lp(u):
        a, bG, bM, us = u
        s2 = exp(us)
        c = {'B': mpf(1), '1': -a, 'M': -bM, 'G': -bG}
        quad = sum(c[p] * c[q] * Q[(p, q)] for p in names for q in names)
        t = -s2 + us
        t += -(bM / mpf('0.5')) ** 2 / 2 - (bG / mpf('0.5')) ** 2 / 2 - a * a / 2
        t += -N * log(s2) / 2 - logdetR / 2 - quad / (2 * s2)
        return t
    return lp, 4

def m14_11_ref(dps):
    d, N, B, G, M, one = gram_model('ch14_m14_11', 0)
    D = [[mpf(x) for x in row] for row in d['Dmat']]
    delta = mpf(0.01)
    def full(u):
        a, bG, bM, ue, ur = u
        es = exp(ue); rs = exp(ur)
        K = [[None] * N for _ in range(N)]
        for i in range(N - 1):
            K[i][i] = es + delta
            for j in range(i + 1, N):
                K[i][j] = es * exp(-rs * D[i][j]); K[j][i] = K[i][j]
        K[N - 1][N - 1] = es + delta
        L = chol(K, N)
        logdet = 2 * sum(log(L[i][i]) for i in range(N))
        r = [B[i] - (a + bM * M[i] + bG * G[i]) for i in range(N)]
        alpha = solve_chol(L, N, r)
        quad = sum(r[i] * alpha[i] for i in range(N))
        t = ue + ur
        t += -((rs - 3) / mpf('0.25')) ** 2 / 2 - ((es - 1) / mpf('0.25')) ** 2 / 2
        t += -(bM / mpf('0.5')) ** 2 / 2 - (bG / mpf('0.5')) ** 2 / 2 - a * a / 2
        t += -logdet / 2 - quad / 2
        return t, L, alpha, es, rs
    def lp(u): return full(u)[0]
    def grad(u):
        t, L, alpha, es, rs = full(u)
        a, bG, bM, ue, ur = u
        Si = inv_chol(L, N)
        ga = sum(alpha); gM = sum(alpha[i] * M[i] for i in range(N)); gG = sum(alpha[i] * G[i] for i in range(N))
        tr_e = tr_r = aq_e = aq_r = mpf(0)
        for i in range(N):
            tr_e += Si[i][i]; aq_e += alpha[i] * alpha[i]
            for j in range(N):
                if i == j: continue
                x = 1 if False else None
                e = exp(-rs * D[min(i, j)][max(i, j)])
                dE = e; dR = -es * D[min(i, j)][max(i, j)] * e
                tr_e += Si[i][j] * dE; tr_r += Si[i][j] * dR
                aq_e += alpha[i] * alpha[j] * dE; aq_r += alpha[i] * alpha[j] * dR
        # d/dEta: lp terms
        g_es = -(es - 1) / mpf('0.0625') + (-tr_e + aq_e) / 2
        g_rs = -(rs - 3) / mpf('0.0625') + (-tr_r + aq_r) / 2
        return [ga - a, gG + (-bG / mpf('0.25')), gM + (-bM / mpf('0.25')), 1 + g_es * es, 1 + g_rs * rs]
    return lp, grad, 5
