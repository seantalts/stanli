import json
import math
import sys

from mpmath import mp, mpf

import mpdens
import mptrans
from mpfun import F, divide, minus, plus
from mpvals import (Mat, RVec, StanReject, Unsupported, Vec, copy, flat, is_scalar, map1,
                    map2, mat_cols, matmul, to_real, transpose)
from sexp import field


class Return(Exception):
    def __init__(self, v):
        self.v = v


class Break(Exception):
    pass


class Continue(Exception):
    pass


def pat(n):
    return n[0][1]


def meta(n):
    return n[1][1]


def ty_of(n):
    for item in meta(n):
        if item[0] == "type_":
            return item[1]


def ad_of(n):
    for item in meta(n):
        if item[0] == "adlevel":
            return item[1] == "AutoDiffable"
    return False


def opt(x):
    return x[0] if x else None


def jflat(x, want_int):
    if isinstance(x, list):
        dims = []
        t = x
        while isinstance(t, list):
            dims.append(len(t))
            if not t:
                break
            t = t[0]
        total = 1
        for d in dims:
            total *= d
        out = [None] * total
        stride = [1]
        for d in dims[:-1]:
            stride.append(stride[-1] * d)

        def go(node, depth, off):
            if depth == len(dims):
                out[off] = node
                return
            for i, e in enumerate(node):
                go(e, depth + 1, off + i * stride[depth])
        if total:
            go(x, 0, 0)
        vals = out
    else:
        vals = [x]
    res = []
    for e in vals:
        if isinstance(e, str):
            e = {"Inf": math.inf, "-Inf": -math.inf, "NaN": math.nan, "inf": math.inf,
                 "-inf": -math.inf, "nan": math.nan}[e]
        if want_int:
            res.append(int(e))
        else:
            res.append(mpf(e))
    return res


def sel(idx, n):
    k = idx[0]
    if k == "S":
        return idx[1]
    if k == "A":
        return list(range(1, n + 1))
    if k == "U":
        return list(range(idx[1], n + 1))
    if k == "D":
        return list(range(1, idx[1] + 1))
    if k == "B":
        return list(range(idx[1], idx[2] + 1))
    if k == "M":
        return list(idx[1])
    raise Unsupported("index " + k)


def get_idx(base, idxs):
    if not idxs:
        return base
    if isinstance(base, Mat):
        rs = sel(idxs[0], len(base))
        if len(idxs) == 1:
            if isinstance(rs, int):
                return RVec(base[rs - 1])
            return Mat([list(base[r - 1]) for r in rs])
        cs = sel(idxs[1], mat_cols(base))
        if isinstance(rs, int) and isinstance(cs, int):
            return base[rs - 1][cs - 1]
        if isinstance(rs, int):
            return RVec([base[rs - 1][c - 1] for c in cs])
        if isinstance(cs, int):
            return Vec([base[r - 1][cs - 1] for r in rs])
        return Mat([[base[r - 1][c - 1] for c in cs] for r in rs])
    s = sel(idxs[0], len(base))
    if isinstance(s, int):
        if s < 1 or s > len(base):
            raise StanReject("index out of range")
        return get_idx(base[s - 1], idxs[1:])
    return type(base)(get_idx(base[k - 1], idxs[1:]) for k in s)


def set_idx(base, idxs, val):
    if not idxs:
        return copy(val)
    if isinstance(base, Mat):
        rs = sel(idxs[0], len(base))
        if len(idxs) == 1:
            if isinstance(rs, int):
                base[rs - 1] = list(val)
            else:
                for r, row in zip(rs, val):
                    base[r - 1] = list(row)
            return base
        cs = sel(idxs[1], mat_cols(base))
        if isinstance(rs, int) and isinstance(cs, int):
            base[rs - 1][cs - 1] = val
        elif isinstance(rs, int):
            for c, v in zip(cs, val):
                base[rs - 1][c - 1] = v
        elif isinstance(cs, int):
            for r, v in zip(rs, val):
                base[r - 1][cs - 1] = v
        else:
            for r, row in zip(rs, val):
                for c, v in zip(cs, row):
                    base[r - 1][c - 1] = v
        return base
    s = sel(idxs[0], len(base))
    if isinstance(s, int):
        if s < 1 or s > len(base):
            raise StanReject("index out of range")
        base[s - 1] = set_idx(base[s - 1], idxs[1:], val)
    else:
        for k, v in zip(s, val):
            base[k - 1] = set_idx(base[k - 1], idxs[1:], v)
    return base


def promote(v, ty):
    if ty in ("UReal", "UVector", "URowVector", "UMatrix") or (
            isinstance(ty, list) and _inner(ty) in ("UReal", "UVector", "URowVector", "UMatrix")):
        return to_real(v)
    return v


def _inner(t):
    while isinstance(t, list) and t and t[0] == "UArray":
        t = t[1]
    return t


def nan_fill(x):
    if isinstance(x, list):
        return type(x)(nan_fill(e) for e in x)
    return mpf("nan")


class Interp:
    def __init__(self, mir, data):
        self.mir = mir
        self.data = data
        self.funcs = {}
        self.target = mpf(0)
        self.theta = None
        self.pos = 0
        self.dyn = [True]
        self.cache = {}
        for fd in field(mir, "functions_block"):
            name = field(fd, "fdname")
            self.funcs[name] = fd
        self.env = {}
        for s in field(mir, "prepare_data"):
            self.stmt(s)(self.env)

    def logp(self, theta):
        self.theta = theta
        self.pos = 0
        self.target = mpf(0)
        env = dict(self.env)
        for s in field(self.mir, "log_prob"):
            self.stmt(s)(env)
        if self.pos != len(theta):
            raise RuntimeError("consumed %d of %d unconstrained" % (self.pos, len(theta)))
        return self.target

    def nparams(self):
        saved = self.theta
        self.theta = None
        self.count_mode = True
        env = dict(self.env)
        self.pos = 0
        self.target = mpf(0)
        self.theta = _Zeros()
        for s in field(self.mir, "log_prob"):
            self.stmt(s)(env)
        n = self.pos
        self.theta = saved
        return n

    def grad_fd(self, theta, h=None):
        mp_dps = mp.dps
        h = h if h is not None else mpf(10) ** (-(mp_dps // 3))
        g = []
        for i in range(len(theta)):
            t = list(theta)
            t[i] = theta[i] + h
            fp = self.logp(t)
            t[i] = theta[i] - h
            fm = self.logp(t)
            g.append((fp - fm) / (2 * h))
        return g

    def stmt(self, node):
        key = id(node)
        c = self.cache.get(key)
        if c is None:
            c = self._stmt(node)
            self.cache[key] = c
        return c

    def expr(self, node):
        key = id(node)
        c = self.cache.get(key)
        if c is None:
            c = self._expr(node)
            self.cache[key] = c
        return c

    def idx(self, i):
        if isinstance(i, str):
            i = [i]
        k = i[0]
        if k == "Single":
            e = self.expr(i[1])
            return lambda env: ("S", e(env)) if not isinstance(e(env), list) else ("M", e(env))
        if k == "All":
            return lambda env: ("A",)
        if k == "Upfrom":
            e = self.expr(i[1])
            return lambda env: ("U", e(env))
        if k == "Downfrom":
            e = self.expr(i[1])
            return lambda env: ("D", e(env))
        if k == "Between":
            a, b = self.expr(i[1]), self.expr(i[2])
            return lambda env: ("B", a(env), b(env))
        if k == "MultiIndex":
            e = self.expr(i[1])
            return lambda env: ("M", e(env))
        raise Unsupported("index kind " + k)

    def _expr(self, node):
        p = pat(node)
        k = p[0]
        if k == "Var":
            name = p[1]

            def var(env):
                try:
                    return env[name]
                except KeyError:
                    raise RuntimeError("unbound " + name)
            return var
        if k == "Lit":
            sort, val = p[1], p[2]
            if sort == "Int":
                v = int(val)
            elif sort == "Real":
                v = mpf(val)
            else:
                v = val
            return lambda env: v
        if k == "Promotion":
            e = self.expr(p[1])
            t = p[2]
            return lambda env: promote(e(env), t)
        if k == "Paren":
            return self.expr(p[1])
        if k == "EAnd":
            a, b = self.expr(p[1]), self.expr(p[2])
            return lambda env: int(bool(a(env)) and bool(b(env)))
        if k == "EOr":
            a, b = self.expr(p[1]), self.expr(p[2])
            return lambda env: int(bool(a(env)) or bool(b(env)))
        if k == "TernaryIf":
            c, a, b = self.expr(p[1]), self.expr(p[2]), self.expr(p[3])
            return lambda env: a(env) if c(env) else b(env)
        if k == "Indexed":
            e = self.expr(p[1])
            idxs = [self.idx(i) for i in p[2]]
            return lambda env: get_idx(e(env), [f(env) for f in idxs])
        if k == "FunApp":
            return self.funapp(node, p)
        if k == "ArrayLit" or k == "ArrayExpr":
            es = [self.expr(e) for e in p[1]]
            return lambda env: [f(env) for f in es]
        if k == "RowVectorExpr":
            es = [self.expr(e) for e in p[1]]

            def rv(env):
                vals = [f(env) for f in es]
                if vals and isinstance(vals[0], RVec):
                    return Mat([list(v) for v in vals])
                return RVec(vals)
            return rv
        raise Unsupported("expr " + k)

    def args_ad(self, args):
        return [self.ad(a) for a in args]

    def ad(self, a):
        return ad_of(a)

    def funapp(self, node, p):
        fn = p[1]
        args = p[2]
        if isinstance(fn, str):
            raise Unsupported("fn " + fn)
        kind = fn[0]
        ev = [self.expr(a) for a in args]
        if kind == "CompilerInternal":
            return self.internal(node, fn, args, ev)
        name = fn[1]
        if kind == "UserDefined":
            return self.user_call(name, fn, args, ev)
        suffix = fn[2]
        flags = [ad_of(a) for a in args]
        if isinstance(suffix, list) and suffix[0] in ("FnLpdf", "FnLpmf"):
            propto = suffix[1] == "true"
            base = name
            for s in ("_lpdf", "_lpmf", "_lupdf", "_lupmf"):
                if base.endswith(s):
                    base = base[: -len(s)]
            return self.density(base, ev, flags, propto)
        if name.endswith("__"):
            return self.operator(name, ev)
        if name.endswith(("_lpdf", "_lpmf", "_lupdf", "_lupmf")):
            base = name.rsplit("_", 1)[0]
            return self.density(base, ev, flags, False)
        if name.endswith(("_lcdf", "_lccdf", "_cdf")):
            return self.cdf(name, ev)
        f = F.get(name)
        if f is None:
            raise Unsupported("fn " + name)
        return lambda env: f(*[e(env) for e in ev])

    def cdf(self, name, ev):
        from mpextra import CDFS
        f = CDFS.get(name)
        if f is None:
            raise Unsupported("fn " + name)
        return lambda env: f(*[e(env) for e in ev])

    def density(self, base, ev, flags, propto):
        if base not in mpdens.TERMS:
            if base in EXTRA_DENS:
                g = EXTRA_DENS[base]
                return lambda env: g([e(env) for e in ev], flags, propto)
            raise Unsupported("density " + base)
        dyn = self.dyn

        def run(env):
            fl = [f and dyn[-1] for f in flags]
            return mpdens.lpdf(base, [e(env) for e in ev], fl, propto)
        return run

    def operator(self, name, ev):
        if len(ev) == 1:
            e = ev[0]
            if name == "PMinus__":
                return lambda env: map1(lambda x: -x, e(env))
            if name == "PPlus__":
                return lambda env: e(env)
            if name == "PNot__":
                return lambda env: int(not e(env))
            if name == "Transpose__":
                return lambda env: transpose(e(env))
            raise Unsupported("op " + name)
        a, b = ev
        if name == "Plus__":
            return lambda env: plus(a(env), b(env))
        if name == "Minus__":
            return lambda env: minus(a(env), b(env))
        if name == "Times__":
            def times(env):
                x, y = a(env), b(env)
                if isinstance(x, int) and isinstance(y, int):
                    return x * y
                return matmul(x, y)
            return times
        if name == "Divide__":
            def dv(env):
                x, y = a(env), b(env)
                if isinstance(y, list):
                    raise Unsupported("matrix right-divide")
                return divide(x, y)
            return dv
        if name == "EltTimes__":
            return lambda env: map2(lambda p, q: p * q, a(env), b(env))
        if name == "EltDivide__":
            return lambda env: map2(lambda p, q: p / q if not (isinstance(p, int) and isinstance(q, int)) else divide(p, q), a(env), b(env))
        if name == "Pow__" or name == "EltPow__":
            return lambda env: map2(lambda p, q: mp.power(p, q), a(env), b(env))
        if name == "Modulo__":
            return lambda env: a(env) - b(env) * int(a(env) / b(env)) if False else int(math.fmod(a(env), b(env)))
        cmpops = {"Less__": lambda x, y: x < y, "Leq__": lambda x, y: x <= y,
                  "Greater__": lambda x, y: x > y, "Geq__": lambda x, y: x >= y,
                  "Equals__": lambda x, y: x == y, "NEquals__": lambda x, y: x != y}
        if name in cmpops:
            f = cmpops[name]
            return lambda env: int(f(a(env), b(env)))
        raise Unsupported("op " + name)

    def internal(self, node, fn, args, ev):
        what = fn[1]
        if isinstance(what, list) and what[0] == "FnReadParam":
            return self.read_param(node, what)
        if what == "FnReadData":
            name = args[0][0][1][2]
            t = ty_of(node)
            want_int = _inner(t) == "UInt"

            def rd(env):
                if name not in self.data:
                    raise RuntimeError("missing data " + name)
                return jflat(self.data[name], want_int)
            return rd
        if what == "FnNegInf":
            return lambda env: mpf("-inf")
        if what == "FnLength":
            return lambda env: len(ev[0](env))
        if what == "FnMakeArray":
            return lambda env: [e(env) for e in ev]
        if what == "FnMakeRowVec":
            def mk(env):
                vals = [e(env) for e in ev]
                if vals and isinstance(vals[0], RVec):
                    return Mat([list(v) for v in vals])
                return RVec(vals)
            return mk
        raise Unsupported("internal " + (what if isinstance(what, str) else what[0]))

    def read_param(self, node, what):
        cons = field(what, "constrain")
        dims_e = [self.expr(d) for d in field(what, "dims")] if field(what, "dims") else []
        t = ty_of(node)
        kind = cons if isinstance(cons, str) else cons[0]
        bexpr = [self.expr(b) for b in cons[1:]] if not isinstance(cons, str) else []
        na = 0
        bt = t
        while isinstance(bt, list) and bt[0] == "UArray":
            na += 1
            bt = bt[1]
        el = mptrans.elementwise(kind, bexpr)

        def run(env):
            dims = [d(env) for d in dims_e]
            adims, idims = dims[:na], dims[na:]
            bvals = [e(env) for e in bexpr]
            bflat = [flat(b) if isinstance(b, list) else None for b in bvals]
            ctr = [0]
            th = self.theta

            def take(n):
                s = [th[self.pos + i] for i in range(n)]
                self.pos += n
                return s

            def bnd():
                i = ctr[0]
                ctr[0] += 1
                return [bv if bf is None else bf[i] for bv, bf in zip(bvals, bflat)]

            def leaf():
                if el is not None:
                    n = 1
                    for d in idims:
                        n *= d
                    xs = []
                    for u in take(n):
                        x, j = el(u, bnd())
                        self.target += j
                        xs.append(x)
                    return shape(xs)
                n = idims[0] if idims else 1
                if kind == "Ordered":
                    x, j = mptrans.ordered(take(n))
                elif kind == "PositiveOrdered":
                    x, j = mptrans.positive_ordered(take(n))
                elif kind == "Simplex":
                    x, j = mptrans.simplex(take(n - 1))
                elif kind == "SumToZero":
                    x, j = mptrans.sum_to_zero(take(n - 1))
                elif kind == "CholeskyCorr":
                    m, j = mptrans.cholesky_corr(take(n * (n - 1) // 2), n)
                    self.target += j
                    return m
                elif kind == "Correlation":
                    m, j = mptrans.corr_matrix(take(n * (n - 1) // 2), n)
                    self.target += j
                    return m
                elif kind == "CholeskyCov":
                    M, N = idims[0], idims[1]
                    m, j = mptrans.cholesky_cov(take(N * (N + 1) // 2 + (M - N) * N), M, N)
                    self.target += j
                    return m
                else:
                    raise Unsupported("transform " + kind)
                self.target += j
                return Vec(x) if bt != "URowVector" else RVec(x)

            def shape(xs):
                if bt == "UReal":
                    return xs[0]
                if bt == "UVector":
                    return Vec(xs)
                if bt == "URowVector":
                    return RVec(xs)
                if bt == "UMatrix":
                    r, c = idims
                    return Mat([[xs[j * r + i] for j in range(c)] for i in range(r)])
                raise Unsupported("shape " + str(bt))

            def build(level):
                if level == na:
                    return leaf()
                return [build(level + 1) for _ in range(adims[level])]
            return build(0)
        return run

    def user_call(self, name, fn, args, ev):
        fd = self.funcs.get(name)
        if fd is None:
            raise Unsupported("udf " + name)
        fargs = field(fd, "fdargs")
        body = field(fd, "fdbody")
        flags = [ad_of(a) for a in args]
        stmts = [self.stmt(s) for s in body]
        names = [a[1] for a in fargs]
        fad = [a[0] == "AutoDiffable" for a in fargs]

        def call(env):
            local = {n: copy(e(env)) for n, e in zip(names, ev)}
            dyn = any(f and a for f, a in zip(flags, fad))
            self.dyn.append(dyn and self.dyn[-1])
            try:
                for s in stmts:
                    s(local)
            except Return as r:
                return r.v
            finally:
                self.dyn.pop()
            return None
        return call

    def decl_default(self, s):
        if isinstance(s, str):
            s = [s]
        k = s[0]
        if k == "SInt":
            return lambda env: 0
        if k == "SReal":
            return lambda env: mpf("nan")
        if k == "SVector":
            e = self.expr(s[2])
            return lambda env: Vec([mpf("nan")] * e(env))
        if k == "SRowVector":
            e = self.expr(s[2])
            return lambda env: RVec([mpf("nan")] * e(env))
        if k == "SMatrix":
            r, c = self.expr(s[2]), self.expr(s[3])
            return lambda env: Mat([[mpf("nan")] * c(env) for _ in range(r(env))])
        if k == "SArray":
            inner = self.decl_default(s[1])
            n = self.expr(s[2])
            return lambda env: [inner(env) for _ in range(n(env))]
        raise Unsupported("sized " + k)

    def _stmt(self, node):
        p = pat(node)
        k = p[0]
        if k == "Skip":
            return lambda env: None
        if k == "Block" or k == "SList":
            ss = [self.stmt(s) for s in p[1]]

            def blk(env):
                for s in ss:
                    s(env)
            return blk
        if k == "Decl":
            name = field(p, "decl_id")
            dt = field(p, "decl_type")
            init = field(p, "initialize")
            if init == "Default" or init == "Uninit" or (isinstance(init, list) and init[0] == "Uninit"):
                if dt[0] == "Sized":
                    d = self.decl_default(dt[1])

                    def decl(env):
                        env[name] = d(env)
                    return decl
                return lambda env: env.__setitem__(name, None)
            e = self.expr(init[1])

            def decl2(env):
                env[name] = copy(e(env))
            return decl2
        if k == "Assignment":
            lv = p[1]
            rhs = self.expr(p[3])
            if lv[0][0] != "LVariable":
                raise Unsupported("lvalue " + lv[0][0])
            name = lv[0][1]
            idxs = [self.idx(i) for i in lv[1]]
            if not idxs:
                def asg(env):
                    env[name] = copy(rhs(env))
                return asg

            def asg2(env):
                v = rhs(env)
                env[name] = set_idx(env[name], [f(env) for f in idxs], v)
            return asg2
        if k == "TargetPE" or k == "JacobianPE":
            e = self.expr(p[1])

            def tpe(env):
                v = e(env)
                self.target += v
            return tpe
        if k == "For":
            var = field(p, "loopvar")
            lo = self.expr(field(p, "lower"))
            hi = self.expr(field(p, "upper"))
            body = self.stmt(field(p, "body"))

            def loop(env):
                a, b = lo(env), hi(env)
                for i in range(a, b + 1):
                    env[var] = i
                    try:
                        body(env)
                    except Continue:
                        pass
                    except Break:
                        break
            return loop
        if k == "While":
            c = self.expr(p[1])
            body = self.stmt(p[2])

            def wh(env):
                while c(env):
                    try:
                        body(env)
                    except Continue:
                        pass
                    except Break:
                        break
            return wh
        if k == "IfElse":
            c = self.expr(p[1])
            a = self.stmt(p[2])
            b = self.stmt(p[3][0]) if p[3] else None

            def ife(env):
                if c(env):
                    a(env)
                elif b is not None:
                    b(env)
            return ife
        if k == "Return":
            e = self.expr(p[1][0]) if p[1] else None

            def ret(env):
                raise Return(e(env) if e else None)
            return ret
        if k == "Break":
            def br(env):
                raise Break()
            return br
        if k == "Continue":
            def co(env):
                raise Continue()
            return co
        if k == "Profile":
            ss = [self.stmt(s) for s in p[2]]

            def prof(env):
                for s in ss:
                    s(env)
            return prof
        if k == "NRFunApp":
            fn = p[1]
            if fn[0] == "CompilerInternal":
                what = fn[1]
                nm = what if isinstance(what, str) else what[0]
                if nm == "FnReject" or nm == "FnFatalError":
                    def rj(env):
                        raise StanReject("reject")
                    return rj
                return lambda env: None
            if fn[0] == "UserDefined":
                ev = self.funapp_node_nr(p)
                return ev
            return lambda env: None
        raise Unsupported("stmt " + k)

    def funapp_node_nr(self, p):
        fn = p[1]
        args = p[2]
        ev = [self.expr(a) for a in args]
        return self.user_call(fn[1], fn, args, ev)


from mpextra import EXTRA_DENS  # noqa: E402


class _Zeros:
    def __getitem__(self, i):
        return mpf(0)

    def __len__(self):
        return 1 << 30
