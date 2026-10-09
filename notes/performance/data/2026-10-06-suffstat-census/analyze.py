#!/usr/bin/env python3
"""Sufficient-statistic / duplicate-row census over one graph dump.

Throwaway spike script.  Reads a graph_dump file (census configuration) and
does element-level value numbering: every slot element gets a 64-bit key
that is equal for two elements iff they are the same expression over the
same parameter elements and the same data values (up to hash collisions).

  key   full expression (data leaves hashed by value)
  skel  same expression with every data leaf replaced by one wildcard
  flag  ACTIVE, LEAF (a parameter element, possibly bound-constrained),
        MATVEC (contains a data-matrix * parameter-vector row), OPAQUE

Density elements are then grouped:
  shape (b)  by the full key of all arguments including the variate;
  shape (a)  by the key of the non-variate arguments, for exponential-family
             opcodes whose variate is data.
A reverse demand pass counts how many elements each op still has to compute
when only one representative per group survives at the linear frontier
(the inputs of the target's sums).
"""
import collections
import gzip
import json
import pathlib
import re
import sys

import numpy as np

np.seterr(over="ignore")
U = np.uint64
HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]

ACTIVE, LEAF, MATVEC, OPAQUE = 1, 2, 4, 8
C1, C2 = U(0x9E3779B97F4A7C15), U(0xD1B54A32D192ED03)
M1, M2 = U(0xBF58476D1CE4E5B9), U(0x94D049BB133111EB)


def mix(a, b):
    z = np.asarray(a, dtype=U) * C1 + np.asarray(b, dtype=U) + C2
    z = (z ^ (z >> U(30))) * M1
    z = (z ^ (z >> U(27))) * M2
    return z ^ (z >> U(31))


def shash(s):
    h = U(1469598103934665603)
    for ch in s.encode():
        h = mix(h, U(ch))
    return U(h)


def vechash(a):
    if a.size == 1:
        return mix(U(77), a[0])
    idx = np.arange(a.size, dtype=U)
    return mix(U(a.size), np.bitwise_xor.reduce(mix(a, idx)))


def macro_ops(text, name):
    m = re.search(r"#define " + name + r"\(X\)(.*?)\n\n", text, re.S)
    return set(re.findall(r"X\((OP_[A-Z0-9_]+)", m.group(1))) if m else set()


_opt = (REPO / "runtime/include/stanli/optable.hpp").read_text()
ELEMENTWISE = (macro_ops(_opt, "STANLI_SCALAR_UNARY_LIST")
               | macro_ops(_opt, "STANLI_SCALAR_BINARY_LIST")
               | {"OP_EXP", "OP_EXPV", "OP_LOGV", "OP_ADD", "OP_SUB", "OP_MUL",
                  "OP_DIV", "OP_POW", "OP_NEG", "OP_INV_LOGIT", "OP_SQRT",
                  "OP_SQUARE", "OP_LOG1M", "OP_TANHV", "OP_TRIGAMMA", "OP_FMA",
                  "OP_BCAST_FMA", "OP_LSE2", "OP_LOG_DIFF_EXP", "OP_LOG_MIX",
                  "OP_LOGIT", "OP_LGAMMA", "OP_DIGAMMA"})
BOUND_CONSTRAIN = {"OP_CONSTRAIN_LOWER", "OP_CONSTRAIN_UPPER", "OP_CONSTRAIN_LU",
                   "OP_CONSTRAIN_OFFSET_MULT"}
LINEAR = {"OP_ADD_N", "OP_SUM_VEC", "OP_ADD", "OP_SUB", "OP_NEG"}
DENS_RE = re.compile(r"^OP_([A-Z0-9_]+)_(LPDF|LPMF|CDF|LCDF|LCCDF)$")
MULTIVAR = re.compile(r"MULTI|LKJ|WISHART|DIRICHLET|GLM|WIENER|GP_|POISSON_BINOMIAL")
# Exponential-family opcodes: a finite sufficient statistic of the variate
# exists for fixed remaining arguments.  Value: int groups after the first
# that are also summed (binomial trials) rather than part of the group key.
EXPFAM = {"NORMAL_LPDF": 0, "LOGNORMAL_LPDF": 0, "EXPONENTIAL_LPDF": 0,
          "GAMMA_LPDF": 0, "BETA_LPDF": 0, "INV_GAMMA_LPDF": 0,
          "CHI_SQUARE_LPDF": 0, "INV_CHI_SQUARE_LPDF": 0,
          "SCALED_INV_CHI_SQUARE_LPDF": 0, "RAYLEIGH_LPDF": 0,
          "BETA_PROPORTION_LPDF": 0, "VON_MISES_LPDF": 0, "PARETO_LPDF": 0,
          "POISSON_LPMF": 0, "POISSON_LOG_LPMF": 0, "BERNOULLI_LPMF": 0,
          "BERNOULLI_LOGIT_LPMF": 0, "BINOMIAL_LPMF": 1,
          "BINOMIAL_LOGIT_LPMF": 1,
          "NORMAL_ID_GLM_LPDF": 0, "POISSON_LOG_GLM_LPMF": 0,
          "BERNOULLI_LOGIT_GLM_LPMF": 0, "BINOMIAL_LOGIT_GLM_LPMF": 1}
GROUPED_INT = {"OP_BINOMIAL_LPMF", "OP_BINOMIAL_LOGIT_LPMF",
               "OP_BETA_BINOMIAL_LPMF"}
GLM_MARK = None  # scalar-layout tails are detected by shape instead


class Op:
    __slots__ = ("i", "name", "variant", "out", "out2", "ins", "idata")


def load(path):
    slots, ops, vals = [], [], {}
    head = {}
    with gzip.open(path, "rt") as fh:
        for line in fh:
            t = line[0]
            if t == "S":
                p = line.split()
                slots.append((int(p[2]), p[3] == "1", p[4] == "1"))
            elif t == "O":
                p = line.split()
                o = Op()
                o.i, o.name, o.variant = int(p[1]), p[2], int(p[3])
                o.out, o.out2 = int(p[4]), int(p[5])
                n = int(p[6])
                o.ins = [int(x) for x in p[7:7 + n]]
                nid = int(p[7 + n])
                o.idata = np.array(p[8 + n:8 + n + nid], dtype=np.int64)
                ops.append(o)
            elif t == "V":
                p = line.split(" ", 3)
                vals[int(p[1])] = (np.array(p[3].split(), dtype=np.float64)
                                   if int(p[2]) else np.zeros(0))
            elif t == "H":
                p = line.split()
                head = {"nparams": int(p[2]), "result": int(p[4]),
                        "fwd_ok": p[6] == "1"}
    return head, slots, ops, vals


class Analysis:
    def __init__(self, path):
        self.head, self.slots, self.ops, vals = load(path)
        self.vals = vals
        self.K, self.S, self.F = {}, {}, {}
        for s, (ln, is_param, _active) in enumerate(self.slots):
            if is_param:
                idx = np.arange(ln, dtype=U)
                k = mix(mix(U(11), U(s)), idx)
                self.K[s], self.S[s] = k, k
                self.F[s] = np.full(ln, ACTIVE | LEAF, dtype=np.uint8)
        for s, v in vals.items():
            k = mix(U(13), (v + 0.0).view(U) if v.size else np.zeros(0, U))
            self.K[s] = k
            self.S[s] = np.full(v.size, 13, dtype=U)
            self.F[s] = np.zeros(v.size, dtype=np.uint8)
        self.opaque_ops = collections.Counter()
        self.dens = []   # per density op: dict of element arrays
        self.front = {}
        self.forward()

    # ---- forward value numbering -------------------------------------
    def get(self, s):
        if s not in self.K:
            ln = self.slots[s][0]
            self.K[s] = mix(mix(U(17), U(s)), np.arange(ln, dtype=U))
            self.S[s] = np.full(ln, 13, dtype=U)
            self.F[s] = np.zeros(ln, dtype=np.uint8)
        return self.K[s], self.S[s], self.F[s]

    def put(self, s, k, sk, f):
        self.K[s], self.S[s], self.F[s] = k, sk, f

    def elementwise(self, op, L, keep_leaf=False):
        h = mix(shash(op.name), U(op.variant))
        k = np.full(L, h, dtype=U)
        sk = k.copy()
        f = np.zeros(L, dtype=np.uint8)
        allf = np.full(L, 255, dtype=np.uint8)
        for s in op.ins:
            ik, isk, iflag = self.get(s)
            k, sk = mix(k, ik), mix(sk, isk)
            f = f | iflag
            allf = allf & iflag
        if keep_leaf:
            # A bound transform of one parameter element stays a leaf when
            # the bounds are data.
            xf = self.get(op.ins[0])[2]
            others_active = np.zeros(L, dtype=bool)
            for s in op.ins[1:]:
                others_active = others_active | ((self.get(s)[2] & ACTIVE) != 0)
            leaf = ((xf & LEAF) != 0) & ~others_active
            f = np.where(leaf, f | LEAF, f & ~np.uint8(LEAF)).astype(np.uint8)
        else:
            f = (f & ~np.uint8(LEAF)).astype(np.uint8)
        # A data-only element of an active op is data with an unknown value:
        # its skeleton is the wildcard.
        sk = np.where((f & ACTIVE) != 0, sk, U(13))
        return k, sk, f

    def generic(self, op, unique=False):
        h = mix(shash(op.name), U(op.variant))
        hs = h
        if unique:
            h, hs = mix(h, U(op.i + 1000003)), mix(hs, U(op.i + 1000003))
        if op.idata.size:
            hi = vechash(op.idata.astype(U))
            h, hs = mix(h, hi), mix(hs, hi)
        act = False
        for s in op.ins:
            ik, isk, iflag = self.get(s)
            h, hs = mix(h, vechash(ik)), mix(hs, vechash(isk))
            act = act or bool((iflag & ACTIVE).any())
        for o, tag in ((op.out, 0), (op.out2, 1)):
            if o < 0:
                continue
            ln = self.slots[o][0]
            idx = np.arange(ln, dtype=U)
            self.put(o, mix(mix(h, U(tag)), idx), mix(mix(hs, U(tag)), idx),
                     np.full(ln, (ACTIVE | OPAQUE) if act else 0, np.uint8))

    def int_groups(self, op, n_hint):
        """Per-element integer outcome columns of an lpmf/cdf, or None."""
        d = op.idata
        if d.size == 0:
            return []
        if op.name in GROUPED_INT or (d.size not in (1, n_hint) and d[0] in (-1,)):
            groups, p = [], 0
            while p < d.size:
                ln = int(d[p])
                if ln == -1:
                    groups.append(d[p + 1:p + 2])
                    p += 2
                elif ln >= 0 and p + 1 + ln <= d.size:
                    groups.append(d[p + 1:p + 1 + ln])
                    p += 1 + ln
                else:
                    return None
            return groups
        return [d]

    def density(self, op, fam):
        lens = [self.slots[s][0] for s in op.ins]
        is_ordered = fam.startswith("ORDERED_")
        cuts_h = cuts_hs = None
        ins = list(op.ins)
        if is_ordered:
            if op.idata.size < 4:
                return False
            ints = [op.idata[:-3]]
            ck, csk, cf = self.get(ins[1])
            cuts_h, cuts_hs = vechash(ck), vechash(csk)
            cuts_active = bool((cf & ACTIVE).any())
            ins = ins[:1]
            lens = lens[:1]
        else:
            ints = self.int_groups(op, max(lens) if lens else 1)
            if ints is None:
                return False
        N = max(lens + [g.size for g in ints] + [1])
        if any(ln not in (1, N) for ln in lens) or any(g.size not in (1, N) for g in ints):
            return False
        outlen = self.slots[op.out][0]
        if outlen not in (1, N):
            return False
        cols = []  # (key, skel, flag) per argument, broadcast to N
        for g in ints:
            k = mix(U(19), np.broadcast_to(g.astype(U), (N,)))
            cols.append((k, np.full(N, 13, dtype=U), np.zeros(N, np.uint8)))
        for s in ins:
            k, sk, f = self.get(s)
            cols.append((np.broadcast_to(k, (N,)), np.broadcast_to(sk, (N,)),
                         np.broadcast_to(f, (N,))))
        if is_ordered:
            cols.append((np.full(N, cuts_h, U), np.full(N, cuts_hs, U),
                         np.full(N, (ACTIVE | OPAQUE) if cuts_active else 0,
                                 np.uint8)))
        n_var = min(len(ints), 1 + EXPFAM.get(fam, 0)) if ints else 1
        h = mix(shash(op.name), U(op.variant & 0xBF))
        full = np.full(N, h, dtype=U)
        grp = full.copy()
        gsk = full.copy()
        for j, (k, sk, f) in enumerate(cols):
            full = mix(full, k)
            if j >= n_var:
                grp = mix(grp, k)
                gsk = mix(gsk, sk)
        variate_flags = cols[0][2] if cols else np.zeros(N, np.uint8)
        act = np.zeros(N, np.uint8)
        for (_k, _sk, f) in cols:
            act = act | (f & ACTIVE)
        rec = dict(op=op.i, name=op.name, fam=fam, N=N, variant=op.variant,
                   full=full, grp=grp, gsk=gsk, n_var=n_var,
                   variate_active=(variate_flags & ACTIVE) != 0,
                   cols=cols, sum_out=(outlen == 1 and N >= 1), outlen=outlen,
                   kind="scalar")
        self.dens.append(rec)
        if outlen == 1 and N > 1:
            k = vechash(full)
            self.put(op.out, np.array([k], U), np.array([vechash(gsk)], U),
                     np.array([ACTIVE | OPAQUE if act.any() else 0], np.uint8))
        else:
            self.put(op.out, full.copy(), gsk.copy(),
                     np.where(act != 0, ACTIVE | OPAQUE, 0).astype(np.uint8))
        rec["out"] = op.out
        return True

    def glm(self, op, fam):
        d = op.idata
        if fam == "NORMAL_ID_GLM_LPDF":
            rows, cols_n = int(d[0]), int(d[1])
            y, X, alpha, extra = op.ins[0], op.ins[1], op.ins[2], op.ins[4:]
            beta = op.ins[3]
            ints = []
        else:
            X, alpha, beta, extra = op.ins[0], op.ins[1], op.ins[2], op.ins[3:]
            y = None
            rows = self.slots[X][0]
            # idata = [ints..., rows, cols] with an optional 2-element tail.
            tail = None
            for t in (2, 4):
                if d.size >= t and d[d.size - t] * d[d.size - t + 1] == self.slots[X][0] \
                        and d[d.size - t + 1] == self.slots[beta][0] * (
                            1 if fam != "CATEGORICAL_LOGIT_GLM_LPMF" else 0) or (
                        d.size >= t and fam == "CATEGORICAL_LOGIT_GLM_LPMF"
                        and d[d.size - t] > 0 and d[d.size - t + 1] > 0
                        and d[d.size - t] * d[d.size - t + 1] == self.slots[X][0]):
                    tail = t
                    break
            if tail is None:
                return False
            rows, cols_n = int(d[d.size - tail]), int(d[d.size - tail + 1])
            body = d[:d.size - tail]
            if rows == 0 or body.size % rows:
                if body.size == 1:
                    ints = [np.broadcast_to(body, (rows,))]
                else:
                    return False
            else:
                ints = list(body.reshape(-1, rows))
        Xk, _Xs, Xf = self.get(X)
        if (Xf & ACTIVE).any() or Xk.size != rows * cols_n or rows == 0:
            return False
        N = rows
        rowh = np.full(N, 23, dtype=U)
        Xm = Xk.reshape(cols_n, rows)
        for c in range(cols_n):
            rowh = mix(rowh, Xm[c])
        ak, ask, af = self.get(alpha)
        if ak.size not in (1, N):
            return False
        bk, bsk, _bf = self.get(beta)
        bh, bhs = vechash(bk), vechash(bsk)
        h = mix(shash(op.name), U(op.variant))
        grp = mix(mix(mix(np.full(N, h, U), rowh), np.broadcast_to(ak, (N,))), bh)
        gsk = mix(mix(np.full(N, h, U), np.broadcast_to(ask, (N,))), bhs)
        extra_cols = []
        for s in extra:
            k, sk, f = self.get(s)
            if k.size in (1, N):
                grp, gsk = mix(grp, np.broadcast_to(k, (N,))), mix(gsk, np.broadcast_to(sk, (N,)))
            else:
                hk, hsk = vechash(k), vechash(sk)
                grp, gsk = mix(grp, hk), mix(gsk, hsk)
            extra_cols.append(f)
        full = grp.copy()
        n_sum = EXPFAM.get(fam, 0)
        variate_active = np.zeros(N, bool)
        if y is not None:
            yk, _ysk, yf = self.get(y)
            if yk.size not in (1, N):
                return False
            full = mix(full, np.broadcast_to(yk, (N,)))
            variate_active = np.broadcast_to((yf & ACTIVE) != 0, (N,))
        for j, g in enumerate(ints):
            gk = mix(U(19), g.astype(U))
            full = mix(full, gk)
            if j >= 1 + n_sum:
                grp = mix(grp, gk)
        rec = dict(op=op.i, name=op.name, fam=fam, N=N, variant=op.variant,
                   full=full, grp=grp, gsk=gsk, n_var=1,
                   variate_active=variate_active, cols=None, sum_out=True,
                   outlen=1, kind="glm", glm_cols=cols_n, out=op.out,
                   glm_x=X)
        self.dens.append(rec)
        self.put(op.out, np.array([vechash(full)], U), np.array([vechash(gsk)], U),
                 np.array([ACTIVE | OPAQUE], np.uint8))
        return True

    def forward(self):
        slots = self.slots
        self.pre()
        for op in self.ops:
            if not any(slots[s][2] for s in op.ins):
                continue  # inactive op: outputs already have their values
            self.step(op)
            self.post(op)

    def pre(self):
        pass

    def post(self, op):
        pass

    def step(self, op):
        slots = self.slots
        n = op.name
        L = slots[op.out][0] if op.out >= 0 else 0
        lens = [slots[s][0] for s in op.ins]
        m = DENS_RE.match(n)
        if m and "GLM" in n:
            if self.glm(op, n[3:]):
                return
        elif m and (not MULTIVAR.search(n)):
            if self.density(op, n[3:]):
                return
        if n == "OP_INDEX":
            k, sk, f = self.get(op.ins[0])
            i = int(op.idata[0])
            self.put(op.out, k[i:i + 1].copy(), sk[i:i + 1].copy(), f[i:i + 1].copy())
        elif n == "OP_GATHER":
            k, sk, f = self.get(op.ins[0])
            self.put(op.out, k[op.idata], sk[op.idata], f[op.idata])
        elif n == "OP_SLICE":
            k, sk, f = self.get(op.ins[0])
            a = int(op.idata[0])
            self.put(op.out, k[a:a + L].copy(), sk[a:a + L].copy(), f[a:a + L].copy())
        elif n == "OP_SLICE_STRIDED":
            k, sk, f = self.get(op.ins[0])
            ix = int(op.idata[0]) + np.arange(L) * int(op.idata[1])
            self.put(op.out, k[ix], sk[ix], f[ix])
        elif n in ("OP_SET_INDEX", "OP_SET_INDEX_INPLACE", "OP_SET_SLICE",
                   "OP_SET_SLICE_INPLACE", "OP_SET_SLICE_STRIDED",
                   "OP_SET_SLICE_STRIDED_INPLACE"):
            k, sk, f = self.get(op.ins[0])
            vk, vsk, vf = self.get(op.ins[1])
            if not n.endswith("INPLACE") or op.out != op.ins[0]:
                k, sk, f = k.copy(), sk.copy(), f.copy()
            ix = self.write_index(op, vk.size)
            k[ix], sk[ix], f[ix] = vk, vsk, vf
            self.put(op.out, k, sk, f)
        elif n == "OP_CONCAT2":
            a, b = self.get(op.ins[0]), self.get(op.ins[1])
            self.put(op.out, *(np.concatenate([a[j], b[j]]) for j in range(3)))
        elif n == "OP_REP_VEC" and len(op.ins) == 1:
            k, sk, f = self.get(op.ins[0])
            self.put(op.out, np.repeat(k[:1], L), np.repeat(sk[:1], L),
                     np.repeat(f[:1], L))
        elif n == "OP_TRANSPOSE":
            k, sk, f = self.get(op.ins[0])
            r, c = int(op.idata[0]), int(op.idata[1])
            ix = np.arange(r * c).reshape(c, r).T.reshape(-1)
            self.put(op.out, k[ix], sk[ix], f[ix])
        elif n == "OP_MATVEC":
            rows, cols_n = int(op.idata[0]), int(op.idata[1])
            Xk, _Xs, Xf = self.get(op.ins[0])
            bk, bsk, _bf = self.get(op.ins[1])
            if (Xf & ACTIVE).any():
                self.opaque_ops[n] += 1
                self.generic(op)
            else:
                rowh = np.full(rows, 23, dtype=U)
                Xm = Xk.reshape(cols_n, rows)
                for c in range(cols_n):
                    rowh = mix(rowh, Xm[c])
                self.put(op.out, mix(rowh, vechash(bk)),
                         np.full(rows, mix(U(23), vechash(bsk)), U),
                         np.full(rows, ACTIVE | MATVEC, np.uint8))
        elif n in BOUND_CONSTRAIN and all(x in (1, L) for x in lens):
            k, sk, f = self.elementwise(op, L, keep_leaf=True)
            self.put(op.out, k, sk, f)
            if op.out2 >= 0:
                l2 = slots[op.out2][0]
                h = mix(U(29), vechash(k))
                self.put(op.out2, mix(h, np.arange(l2, dtype=U)),
                         mix(h, np.arange(l2, dtype=U)),
                         np.full(l2, ACTIVE | OPAQUE, np.uint8))
        elif n.startswith("OP_CONSTRAIN_"):
            self.generic(op)
            f = self.F[op.out]
            self.F[op.out] = np.full(f.size, ACTIVE | LEAF, np.uint8)
        elif n in ELEMENTWISE and L >= 1 and all(x in (1, L) for x in lens) \
                and op.out2 < 0:
            self.put(op.out, *self.elementwise(op, L))
        elif n in ("OP_ADD_N", "OP_SUM_VEC", "OP_DOT", "OP_LOG_SUM_EXP",
                   "OP_MEAN"):
            self.generic(op)
        elif (n == "OP_CATEGORICAL" or m) and L == 1:
            self.generic(op)
            k = self.K[op.out]
            self.dens.append(dict(
                op=op.i, name=n, fam=n[3:], N=1, variant=op.variant,
                full=k.copy(), grp=k.copy(), gsk=self.S[op.out].copy(),
                n_var=1, variate_active=np.zeros(1, bool), cols=None,
                sum_out=True, outlen=1, kind="generic", out=op.out))
        else:
            self.opaque_ops[n] += 1
            self.generic(op, unique=n in ("OP_ISLAND", "OP_LOOP",
                                          "OP_REGION_MAP", "OP_REDUCE_SUM",
                                          "OP_ODE", "OP_ALGEBRA_SOLVER",
                                          "OP_QUADRATURE", "OP_DAE",
                                          "OP_ODE_ADJOINT"))

    def write_index(self, op, vlen):
        n = op.name
        if "SET_INDEX" in n:
            return np.array([int(op.idata[0])])
        if "STRIDED" in n:
            return int(op.idata[0]) + np.arange(vlen) * int(op.idata[1])
        return int(op.idata[0]) + np.arange(vlen)

    # ---- demand pass --------------------------------------------------
    def weights(self):
        """Work elements per op, used to split an opcode's profile time."""
        w = np.zeros(len(self.ops))
        dens_n = {d["op"]: d["N"] for d in self.dens}
        for op in self.ops:
            n = op.name
            if op.i in dens_n:
                w[op.i] = dens_n[op.i]
            elif n == "OP_ADD_N":
                w[op.i] = len(op.ins)
            elif n in ("OP_SUM_VEC", "OP_DOT", "OP_LOG_SUM_EXP"):
                w[op.i] = self.slots[op.ins[0]][0]
            elif "SET_" in n:
                w[op.i] = self.slots[op.ins[1]][0]
            elif n == "OP_MATVEC":
                w[op.i] = self.slots[op.ins[0]][0]
            else:
                w[op.i] = max(1, self.slots[op.out][0]) if op.out >= 0 else 1
        return w

    def demand(self, mode):
        """Fraction of each op's work still needed.  mode: none|a|b|best."""
        slots, ops = self.slots, self.ops
        D = {}
        dens = {d["op"]: d for d in self.dens}

        def dm(s):
            if s not in D:
                D[s] = np.zeros(slots[s][0], dtype=bool)
            return D[s]

        seen = set()
        ftot = fkept = 0
        frac = np.ones(len(ops))
        if self.head["result"] >= 0:
            dm(self.head["result"])[:] = True
        self.seed(dm, mode)
        for op in reversed(ops):
            if op.out < 0:
                continue
            n = op.name
            active = any(slots[s][2] for s in op.ins)
            d = dm(op.out).copy()
            anyd = bool(d.any()) or (op.out2 >= 0 and bool(dm(op.out2).any()))
            L = d.size
            inplace = n.endswith("INPLACE") and op.out == op.ins[0]
            if not inplace:
                D[op.out] = np.zeros(L, dtype=bool)
            if not active:
                frac[op.i] = 1.0
                continue
            if not anyd:
                frac[op.i] = 0.0
                if inplace:
                    ix = self.write_index(op, slots[op.ins[1]][0])
                    D[op.out][ix] = False
                continue
            rec = dens.get(op.i)
            if rec is not None:
                N = rec["N"]
                if rec["outlen"] == 1 and N > 1 or rec["kind"] == "glm":
                    keys = self.dens_keys(rec, mode)
                    if keys is None:
                        need = np.ones(N, bool)
                    elif isinstance(keys, str):
                        need = np.zeros(N, bool)
                        if not self.lin.get(op.out, False):
                            need[:] = True
                    else:
                        need = np.zeros(N, bool)
                        if self.lin.get(op.out, False):
                            _u, first = np.unique(keys, return_index=True)
                            ftot += N
                            for j in first:
                                kk = int(keys[j])
                                if kk not in seen:
                                    seen.add(kk)
                                    need[j] = True
                                    fkept += 1
                        else:
                            need[:] = True
                else:
                    need = np.broadcast_to(d, (N,)).copy() if d.size in (1, N) \
                        else np.ones(N, bool)
                frac[op.i] = need.sum() / N
                for s in op.ins:
                    x = dm(s)
                    if x.size == N and N > 1:
                        x |= need
                    elif need.any():
                        x[:] = True
                continue
            if n in ("OP_ADD_N", "OP_SUM_VEC"):
                lin = self.lin.get(op.out, False)
                tot = kept = 0
                for s in op.ins:
                    x = dm(s)
                    tot += x.size
                    keys = self.front_keys(s, n, x.size, mode) if lin else None
                    if keys is None:
                        x[:] = True
                        kept += x.size
                        continue
                    ftot += x.size
                    if isinstance(keys, str):
                        continue
                    _u, first = np.unique(keys, return_index=True)
                    for j in first:
                        kk = int(keys[j])
                        if kk not in seen:
                            seen.add(kk)
                            x[j] = True
                            kept += 1
                            fkept += 1
                frac[op.i] = kept / max(tot, 1)
                continue
            frac[op.i] = d.sum() / max(L, 1)
            if n == "OP_INDEX":
                dm(op.ins[0])[int(op.idata[0])] = True
            elif n == "OP_GATHER":
                dm(op.ins[0])[op.idata[d]] = True
            elif n == "OP_SLICE":
                a = int(op.idata[0])
                dm(op.ins[0])[a:a + L] |= d
            elif n == "OP_SLICE_STRIDED":
                ix = int(op.idata[0]) + np.arange(L) * int(op.idata[1])
                dm(op.ins[0])[ix[d]] = True
            elif "SET_" in n and len(op.ins) == 2:
                vlen = slots[op.ins[1]][0]
                ix = self.write_index(op, vlen)
                dm(op.ins[1])[:] |= d[ix]
                frac[op.i] = d[ix].sum() / max(vlen, 1)
                if inplace:
                    D[op.out][ix] = False
                else:
                    rest = d.copy()
                    rest[ix] = False
                    dm(op.ins[0])[:] |= rest
            elif n == "OP_CONCAT2":
                a = slots[op.ins[0]][0]
                dm(op.ins[0])[:] |= d[:a]
                dm(op.ins[1])[:] |= d[a:]
            elif n == "OP_REP_VEC" and len(op.ins) == 1:
                dm(op.ins[0])[0] = True
            elif n == "OP_TRANSPOSE":
                r, c = int(op.idata[0]), int(op.idata[1])
                ix = np.arange(r * c).reshape(c, r).T.reshape(-1)
                dm(op.ins[0])[ix[d]] = True
            elif n == "OP_MATVEC" and (self.Ffinal[op.out] & MATVEC).all():
                dm(op.ins[1])[:] = True
            elif (n in ELEMENTWISE or n in BOUND_CONSTRAIN) and all(
                    slots[s][0] in (1, L) for s in op.ins):
                if op.out2 >= 0 and dm(op.out2).any():
                    d = np.ones(L, bool)
                    frac[op.i] = 1.0
                for s in op.ins:
                    x = dm(s)
                    if x.size == L and L > 1:
                        x |= d
                    else:
                        x[:] = True
            else:
                frac[op.i] = 1.0
                for s in op.ins:
                    dm(s)[:] = True
        if isinstance(mode, str):
            self.front[mode] = (int(ftot), int(fkept))
        return frac

    def seed(self, dm, mode):
        pass

    def dens_keys(self, rec, mode):
        """None: every element needed; array: one representative per key."""
        if mode in ("none", "cse") or (mode == "a" and not rec["eligible_a"]):
            return None
        return rec["grp"] if (mode in ("a", "best") and rec["eligible_a"]) \
            else rec["full"]

    def front_keys(self, s, opname, size, mode):
        dd = self.out_dens.get(s)
        a_ok = dd is not None and dd["eligible_a"] and dd["outlen"] == dd["N"]
        if mode == "none" or (mode == "a" and not a_ok) \
                or (mode == "cse" and (opname != "OP_ADD_N" or size != 1)):
            return None
        if a_ok and mode in ("a", "best"):
            return dd["grp"]
        return self.Kfinal[s]

    def prepare(self):
        """Linear-to-target map, final keys, per-density eligibility."""
        slots, ops = self.slots, self.ops
        consumers = collections.defaultdict(list)
        for op in ops:
            for s in op.ins:
                consumers[s].append(op)
        lin = {}
        res = self.head["result"]
        for op in reversed(ops):
            for o in (op.out,):
                if o < 0 or o in lin:
                    continue
                if o == res:
                    lin[o] = True
                    continue
                cs = consumers.get(o, [])
                ok = bool(cs)
                for c in cs:
                    if c.name in LINEAR and lin.get(c.out, False):
                        continue
                    if c.name in ("OP_MUL", "OP_DIV") and lin.get(c.out, False) \
                            and all((not slots[s][2]) for s in c.ins if s != o):
                        continue
                    ok = False
                    break
                lin[o] = ok
        self.lin = lin
        self.Kfinal, self.Ffinal = self.K, self.F
        self.out_dens = {}
        for d in self.dens:
            d["lin"] = lin.get(d["out"], False)
            d["variate_data"] = not bool(np.asarray(d["variate_active"]).any())
            d["eligible_a"] = (d["fam"] in EXPFAM and d["variate_data"])
            self.out_dens[d["out"]] = d


def classify(cols_list, n_var):
    """Provenance class of each non-variate argument across a term."""
    out = []
    if not cols_list or cols_list[0] is None:
        return ["linpred"]
    nargs = len(cols_list[0])
    for j in range(nargs):
        k = np.concatenate([c[j][0] for c in cols_list])
        sk = np.concatenate([c[j][1] for c in cols_list])
        f = np.concatenate([c[j][2] for c in cols_list])
        act = (f & ACTIVE) != 0
        if not act.any():
            cls = "data"
        elif not act.all():
            cls = "mixed-data/active"
        else:
            nk, nsk = np.unique(k).size, np.unique(sk).size
            if nk == 1:
                cls = "bcast"
            elif (f & LEAF).all():
                cls = "gather"
            elif (f & OPAQUE).any():
                cls = "other"
            elif (f & MATVEC).any():
                cls = "linpred"
            elif nk > nsk:
                cls = "covar"
            else:
                cls = "fn(gather)"
        out.append(("y:" if j < n_var else "") + cls)
    return out


def analyze(name, prof=None):
    A = Analysis(HERE / "out" / name / "census.txt.gz")
    A.prepare()
    w = A.weights()
    # profile time per op
    t = np.zeros(len(A.ops))
    covered = 0.0
    if prof:
        byname = collections.defaultdict(list)
        for op in A.ops:
            byname[op.name].append(op.i)
        for oc, p in prof.items():
            ids = byname.get(oc)
            if not ids:
                continue
            ww = w[ids]
            tt = p["fwd"] + p["bwd"]
            t[ids] = tt * ww / max(ww.sum(), 1e-300)
            covered += tt
    total_t = sum(p["fwd"] + p["bwd"] for p in prof.values()) if prof else 0.0
    res = {"name": name, "n_ops": len(A.ops), "fwd_ok": A.head["fwd_ok"],
           "opaque_ops": dict(A.opaque_ops), "prof_total": total_t,
           "prof_covered": covered}
    fr = {m: A.demand(m) for m in ("none", "a", "b", "best", "cse")}
    res["frontier"] = A.front
    for m in ("a", "b", "best", "cse"):
        if total_t > 0:
            base = float((t * fr["none"]).sum()) + (total_t - covered)
            new = float((t * fr[m]).sum()) + (total_t - covered)
            res["ceil_" + m] = base / max(new, 1e-9)
            res["saved_share_" + m] = 1 - new / base
        wb, wn = float((w * fr["none"]).sum()), float((w * fr[m]).sum())
        res["elem_ratio_" + m] = wb / max(wn, 1e-9)
    # terms: group density ops by opcode / variant / variate-data / lin
    groups = collections.defaultdict(list)
    for d in A.dens:
        groups[(d["name"], d["variant"] & 0x3F, d["variate_data"], d["lin"],
                d["kind"])].append(d)
    dens_elems = sum(d["N"] for d in A.dens)
    dens_time = float(sum(t[d["op"]] for d in A.dens))
    terms = []
    for (opn, _var, vdata, lin, kind), ds in groups.items():
        N = sum(d["N"] for d in ds)
        full = np.concatenate([d["full"] for d in ds])
        grp = np.concatenate([d["grp"] for d in ds])
        gsk = np.concatenate([d["gsk"] for d in ds])
        fam = ds[0]["fam"]
        cls = classify([d["cols"] for d in ds], ds[0]["n_var"]) \
            if kind == "scalar" else ["linpred" if kind == "glm" else "opaque"]
        tt = float(sum(t[d["op"]] for d in ds))
        terms.append(dict(
            op=opn[3:], n_ops=len(ds), N=int(N), variate_data=bool(vdata),
            lin=bool(lin), kind=kind, expfam=fam in EXPFAM, classes=cls,
            G_b=int(np.unique(full).size), G_grp=int(np.unique(grp).size),
            G_skel=int(np.unique(gsk).size),
            elem_share=N / max(dens_elems, 1),
            time_share=tt / total_t if total_t else None,
            glm_cols=ds[0].get("glm_cols")))
    res["terms"] = terms
    res["dens_elems"] = int(dens_elems)
    res["dens_time_share"] = dens_time / total_t if total_t else None
    return res


def main():
    profs = {}
    pf = HERE / "stage2_profiles.json"
    if pf.exists():
        for r in json.loads(pf.read_text()):
            if r.get("status") == "ok":
                profs[r["name"]] = r
    names = sys.argv[1:]
    if not names:
        names = sorted(p.name for p in (HERE / "out").iterdir()
                       if (p / "census.txt.gz").exists())
    out = []
    for nm in names:
        try:
            r = analyze(nm, profs.get(nm, {}).get("census_prof"))
            if nm in profs:
                r["default_ns"] = profs[nm]["default_ns"]
                r["census_ns"] = profs[nm]["census_ns"]
        except Exception as e:  # noqa: BLE001
            import traceback
            r = {"name": nm, "error": f"{type(e).__name__}: {e}",
                 "trace": traceback.format_exc()[-800:]}
        out.append(r)
        print(nm, r.get("error") or
              f"ceil a={r.get('ceil_a', 0):.2f} b={r.get('ceil_b', 0):.2f} "
              f"best={r.get('ceil_best', 0):.2f}", file=sys.stderr, flush=True)
    if len(sys.argv) > 1:
        print(json.dumps(out, indent=1))
    else:
        (HERE / "analysis.json").write_text(json.dumps(out))


if __name__ == "__main__":
    main()
