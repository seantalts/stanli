"""Error metrics, validity and comparison flags for the high-precision reference.

The log density and full-gradient metrics are the fast-mode gate's
(TESTING.md#fast-mode). The directional metric is for models too large for
full finite differences.
"""
import math
import random

GATE = 1e-12
FLOOR = 1e-14
RATIO = 2.0


def finite(x):
    return x - x == 0.0


def lp_error(a, b):
    if a != a and b != b:
        return 0.0
    if a == b:
        return 0.0
    if not (finite(a) and finite(b)):
        return math.inf
    return abs(a - b) / max(abs(a), abs(b), 1.0)


def grad_error(g, ref):
    diff, scale = 0.0, 1.0
    for a, b in zip(g, ref):
        if a != a and b != b:
            continue
        if a == b:
            if finite(a):
                scale = max(scale, abs(a))
            continue
        if not (finite(a) and finite(b)):
            return math.inf
        diff = max(diff, abs(a - b))
        scale = max(scale, abs(a), abs(b))
    return diff / scale


def l2(x):
    return math.sqrt(math.fsum(e * e for e in x))


def direction(seed, n):
    rng = random.Random(seed)
    v = [rng.gauss(0.0, 1.0) for _ in range(n)]
    norm = l2(v)
    return [e / norm for e in v]


def dot(a, b):
    return math.fsum(x * y for x, y in zip(a, b))


def directional_error(g, v, d_hp, scale):
    """|g.v - d_hp| / max(1, scale), with scale = |grad| * |v| and |v| = 1."""
    d = dot(g, v)
    if not (finite(d) and finite(d_hp)):
        return 0.0 if d != d and d_hp != d_hp else math.inf
    return abs(d - d_hp) / max(1.0, scale)


def validity(cmdstan_errs, default_errs, ill):
    errs = list(cmdstan_errs) + list(default_errs)
    if not errs:
        return "unscored"
    if all(e < GATE for e in errs):
        return "valid"
    return "ill_conditioned" if ill else "gate_fail"


def compare(default_err, fast_err):
    if fast_err > RATIO * default_err and fast_err > FLOOR:
        return "fast_worse"
    if default_err > RATIO * fast_err and default_err > FLOOR:
        return "fast_better"
    return "similar"


def percentile(xs, q):
    if not xs:
        return math.nan
    s = sorted(xs)
    k = max(1, math.ceil(q / 100.0 * len(s)))
    return s[k - 1]
