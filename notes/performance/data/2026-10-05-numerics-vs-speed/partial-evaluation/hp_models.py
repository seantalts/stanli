import sys, json, math, pathlib
from mpmath import mp, mpf, log, exp, loggamma as lgamma, diff, log1p
mp.dps = 70
R = str(pathlib.Path(__file__).resolve().parents[1])
sys.path.insert(0, R + '/tools')
import verify_refs as V
MODELS, _ = V.load_refs()

def eval_point(i, variant):
    if variant == 1: return 0.02 * ((i % 5) - 2)
    if variant == 2: return 0.0
    return 0.1 + 0.05 * (i % 7) - 0.15 * (i % 3)

def data(model):
    return json.load(open(f'{R}/tests/brms/{model}.json'))

def student_t(y, nu, m, s):
    nu = mpf(nu); m = mpf(m); s = mpf(s)
    z = (y - m) / s
    return (lgamma((nu + 1) / 2) - lgamma(nu / 2) - log(nu) / 2 - log(mp.pi) / 2
            - log(s) - (nu + 1) / 2 * log1p(z * z / nu))

def normal(y, m, s):
    m = mpf(m); s = mpf(s)
    z = (y - m) / s
    return -log(2 * mp.pi) / 2 - log(s) - z * z / 2

def invlogit(z): return 1 / (1 + exp(-z))

def make_gev(mean_mode='eigen'):
    d = data('s2_gev')
    x = [float(r[1]) for r in d['X']]
    if mean_mode == 'seq': mean = sum(x) / len(x)
    elif mean_mode == 'fsum': mean = math.fsum(x) / len(x)
    elif mean_mode == 'eigen': mean = -0.041406886949022385
    xc = [mpf(xi - mean) for xi in x]
    Y = [mpf(v) for v in d['Y']]
    def lp(u):
        b, ic, ls, txi = u
        sigma = exp(ls)
        mu = [ic + c * b for c in xc]
        xs = [(y - m) / sigma for y, m in zip(Y, mu)]
        bounds = [-1 / min(xs), -1 / max(xs)]
        lb, ub = min(bounds), max(bounds)
        xi = invlogit(txi) * (ub - lb) + lb
        t = mpf(0)
        for y, m in zip(Y, mu):
            xx = (y - m) / sigma
            tt = 1 + xi * xx
            t += -log(sigma) - (1 + 1 / xi) * log(tt) - tt ** (-1 / xi)
        t += student_t(ic, 3, '-0.3', '2.5')
        t += student_t(sigma, 3, 0, '2.5') - log(mpf(1) / 2)
        t += normal(txi, 0, '2.5')
        t += ls
        return t
    return lp, 4

def make_me(model):
    d = data(model)
    Y = [mpf(v) for v in d['Y']]
    two = model == 's2_me2_nomecor'
    Xn = [[mpf(v) for v in d['Xn_1']]] + ([[mpf(v) for v in d['Xn_2']]] if two else [])
    nz = [[mpf(v) for v in d['noise_1']]] + ([[mpf(v) for v in d['noise_2']]] if two else [])
    K = len(Xn); N = len(Y)
    ic0 = '-0.3' if two else '0.2'
    def lp(u):
        ic = u[0]; bsp = u[1:1 + K]; ls = u[1 + K]
        mm = u[2 + K:2 + 2 * K]; lsd = u[2 + 2 * K:2 + 3 * K]
        z = [u[2 + 3 * K + k * N: 2 + 3 * K + (k + 1) * N] for k in range(K)]
        sigma = exp(ls); sd = [exp(v) for v in lsd]
        Xme = [[mm[k] + sd[k] * zi for zi in z[k]] for k in range(K)]
        t = mpf(0)
        for n in range(N):
            mu = ic + sum(bsp[k] * Xme[k][n] for k in range(K))
            t += normal(Y[n], mu, sigma)
        t += student_t(ic, 3, ic0, '2.5')
        t += student_t(sigma, 3, 0, '2.5') - log(mpf(1) / 2)
        for k in range(K):
            for n in range(N):
                t += normal(Xn[k][n], Xme[k][n], nz[k][n])
                t += -log(2 * mp.pi) / 2 - z[k][n] ** 2 / 2
        t += ls + sum(lsd)
        return t
    return lp, 2 + 3 * K + K * N

def build(model, **kw):
    if model == 's2_gev': return make_gev(**kw)
    return make_me(model)

def hp(model, point, dps=70, **kw):
    mp.dps = dps
    fn, n = build(model, **kw)
    u = [mpf(eval_point(i, point)) for i in range(n)]
    val = fn(u)
    grad = []
    for i in range(n):
        order = [0] * n; order[i] = 1
        grad.append(diff(lambda *a: fn(list(a)), u, tuple(order)))
    mp.dps = 70
    return [val] + grad

def ulp_of(v):
    f = float(v)
    return abs(math.nextafter(f, math.inf) - f)

def uerr(x, h):
    return float(abs(mpf(x) - h) / ulp_of(h))

def hp_wa(model, point, dps=70, doubles=None):
    mp.dps = dps
    d = data(model)
    out = {}
    D = (lambda n, v: mpf(doubles[n]) if doubles else v)
    if model == 's2_gev':
        u = [mpf(eval_point(i, point)) for i in range(4)]
        sigma = D('sigma', exp(u[2])); b = D('b.1', u[0]); ic = D('Intercept', u[1]); txi = D('tmp_xi', u[3])
        out['b.1'] = u[0]; out['Intercept'] = u[1]; out['sigma'] = exp(u[2]); out['tmp_xi'] = u[3]
        out['lprior'] = (student_t(ic, 3, '-0.3', '2.5') + student_t(sigma, 3, 0, '2.5')
                         - log(mpf(1) / 2) + normal(txi, 0, '2.5'))
        out['b_Intercept'] = ic - mpf(-0.041406886949022385) * b
    else:
        two = model == 's2_me2_nomecor'
        K = 2 if two else 1; N = 40
        n = 2 + 3 * K + K * N
        u = [mpf(eval_point(i, point)) for i in range(n)]
        ic = u[0]; bsp = u[1:1 + K]; ls = u[1 + K]
        mm = u[2 + K:2 + 2 * K]; lsd = u[2 + 2 * K:2 + 3 * K]
        z = [u[2 + 3 * K + k * N: 2 + 3 * K + (k + 1) * N] for k in range(K)]
        sigma = exp(ls); sd = [exp(v) for v in lsd]
        out['Intercept'] = ic
        if doubles:
            ic = mpf(doubles['Intercept']); sigma = mpf(doubles['sigma'])
            mm = [mpf(doubles['meanme_1.%d' % (k + 1)]) for k in range(K)]
            sd = [mpf(doubles['sdme_1.%d' % (k + 1)]) for k in range(K)]
            z = [[mpf(doubles['zme_%d.%d' % (k + 1, i + 1)]) for i in range(N)] for k in range(K)]
        for k in range(K): out['bsp.%d' % (k + 1)] = bsp[k]
        out['sigma'] = sigma
        for k in range(K): out['meanme_1.%d' % (k + 1)] = mm[k]
        for k in range(K): out['sdme_1.%d' % (k + 1)] = sd[k]
        for k in range(K):
            for i in range(N): out['zme_%d.%d' % (k + 1, i + 1)] = z[k][i]
        for k in range(K):
            for i in range(N): out['Xme_%d.%d' % (k + 1, i + 1)] = mm[k] + sd[k] * z[k][i]
        out['lprior'] = (student_t(ic, 3, '-0.3' if two else '0.2', '2.5') + student_t(sigma, 3, 0, '2.5') - log(mpf(1) / 2))
        out['b_Intercept'] = ic
    mp.dps = 70
    return out
