import sys, json, struct, math, subprocess, os, statistics
from mpmath import mp, mpf, log, exp, loggamma as lgamma, diff
mp.dps = 70
import pathlib
R = str(pathlib.Path(__file__).resolve().parents[4])
sys.path.insert(0, R + '/tools')
import verify_refs as V
MODELS, _ = V.load_refs()

def eval_point(i, variant):
    if variant == 1: return 0.02 * ((i % 5) - 2)
    if variant == 2: return 0.0
    return 0.1 + 0.05 * (i % 7) - 0.15 * (i % 3)

def load(model):
    d = json.load(open(f'{R}/tests/brms/{model}.json'))
    x = [float(r[1]) for r in d['X']]
    mean = sum(x) / len(x)
    xc = [xi - mean for xi in x]
    return d['Y'], xc, x

def invlogit(z): return 1 / (1 + exp(-z))

def lp_weibull(u, Y, xc):
    b, ic, ls = u
    shape = exp(ls)
    lpv = mpf(0)
    for y, x in zip(Y, xc):
        mu = invlogit(ic + x * b)
        lpv += log(mu ** (mpf(y) ** shape) - mu ** (mpf(y + 1) ** shape))
    # student_t(Intercept|3,0,2.5)
    nu, s = mpf(3), mpf('2.5')
    z = ic / s
    lpv += lgamma((nu + 1) / 2) - lgamma(nu / 2) - log(nu * mp.pi) / 2 - log(s) - (nu + 1) / 2 * log(1 + z * z / nu)
    a, bt = mpf('0.01'), mpf('0.01')
    lpv += a * log(bt) - lgamma(a) + (a - 1) * log(shape) - bt * shape
    lpv += ls
    return lpv

def lp_hurdle(u, Y, xc):
    b, ic, ls, uh = u
    shape = exp(ls); hu = invlogit(uh)
    lpv = mpf(0)
    for y, x in zip(Y, xc):
        eta = ic + x * b
        if y == 0:
            lpv += log(hu)
        else:
            mu = exp(eta)
            nb = lgamma(y + shape) - lgamma(y + 1) - lgamma(shape) + shape * log(shape / (mu + shape)) + y * log(mu / (mu + shape))
            lpv += log(1 - hu) + nb - log(1 - (shape / (mu + shape)) ** shape)
    nu, s, m0 = mpf(3), mpf('2.5'), mpf('1.1')
    z = (ic - m0) / s
    lpv += lgamma((nu + 1) / 2) - lgamma(nu / 2) - log(nu * mp.pi) / 2 - log(s) - (nu + 1) / 2 * log(1 + z * z / nu)
    a, bt = mpf('0.4'), mpf('0.3')
    lpv += a * log(bt) - lgamma(a) - (a + 1) * log(shape) - bt / shape
    lpv += 0  # beta(1,1)
    lpv += ls + log(hu) + log(1 - hu)
    return lpv

MODEL_FN = {'s2_discrete_weibull': (lp_weibull, 3), 's2_hurdle_negbin': (lp_hurdle, 4)}

def hp(model, point, xc_override=None):
    fn, n = MODEL_FN[model]
    Y, xc, x = load(model)
    if xc_override: xc = xc_override
    xc = [mpf(v) for v in xc]
    u = [mpf(eval_point(i, point)) for i in range(n)]
    f = lambda *a: fn(list(a), Y, xc)
    val = f(*u)
    grad = []
    for i in range(n):
        order = [0] * n; order[i] = 1
        grad.append(diff(f, u, tuple(order)))
    return [val] + grad

def ulp_of(v):
    f = float(v)
    return abs(math.nextafter(f, math.inf) - f)

def uerr(x, h):
    return float(abs(mpf(x) - h) / ulp_of(h))

if __name__ == '__main__':
    for model in MODEL_FN:
        for pt in (0, 1, 2):
            ref = [float(s) for s in MODELS[model]['points'][str(pt)]['values']]
            h = hp(model, pt)
            print(model, pt)
            for i, (r, hv) in enumerate(zip(ref, h)):
                print('  comp', i, 'hp', mp.nstr(hv, 25), 'ref', repr(r), 'cmdstan_ulp %.2f' % uerr(r, hv))
