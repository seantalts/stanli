#!/usr/bin/env python3
"""Follow-up census: linear-Gaussian form, more families, per-family ranking.

Throwaway spike script.  Extends analyze.Analysis with

  * affine forms: every slot element is tracked as  const + sum coef * leaf
    with data coefficients, where a leaf is any active value that is not
    itself affine (a parameter, a constrained parameter, mu + tau * eta, ...)
    identified by its value-number key.  A normal likelihood with a data
    variate, one broadcast sigma and an affine mean is a linear-Gaussian
    term  r = (y - c) - Z theta  with Z an N x P data matrix;
  * group keys for categorical / multinomial / multi_normal / dirichlet ops,
    data-shape weibull, data-phi neg_binomial_2, and poisson with a
    per-observation data offset on the log scale;
  * a demand pass per set of rewrite families, so that estimated ceilings
    can be reported net of the families ranked above.

  .venv/bin/python analyze2.py            # whole corpus -> analysis2.json
  .venv/bin/python analyze2.py NAME ...   # print JSON for some models
"""
import collections
import concurrent.futures
import itertools
import json
import pathlib
import sys

import numpy as np

import analyze as A1
from analyze import ACTIVE, EXPFAM, U, mix, shash, vechash

HERE = pathlib.Path(__file__).resolve().parent
NS_PER_NNZ = 2.0      # assumed cost of one Gram entry, forward + backward
NS_FIXED = 100.0      # assumed fixed cost of the quadratic-form op


class AF:
    """Rows of  const + sum coef * leaf  in CSR layout; ok=False: unknown."""
    __slots__ = ("n", "ptr", "leaf", "coef", "const", "ok")

    def __init__(self, n, ptr, leaf, coef, const, ok):
        self.n, self.ptr, self.leaf, self.coef = n, ptr, leaf, coef
        self.const, self.ok = const, ok

    @staticmethod
    def data(v):
        n = v.size
        return AF(n, np.zeros(n + 1, np.int64), np.zeros(0, U), np.zeros(0),
                  v.astype(np.float64), np.ones(n, bool))

    @staticmethod
    def unknown(n):
        return AF(n, np.zeros(n + 1, np.int64), np.zeros(0, U), np.zeros(0),
                  np.zeros(n), np.zeros(n, bool))

    @staticmethod
    def leaves(keys, active):
        n = keys.size
        a = np.asarray(active, bool)
        cnt = a.astype(np.int64)
        return AF(n, np.concatenate([[0], np.cumsum(cnt)]), keys[a].astype(U),
                  np.ones(int(cnt.sum())), np.zeros(n), a.copy())

    @staticmethod
    def coo(n, rows, leaf, coef, const, ok):
        o = np.argsort(rows, kind="stable")
        ptr = np.concatenate([[0], np.cumsum(np.bincount(rows, minlength=n))])
        return AF(n, ptr.astype(np.int64), leaf[o], coef[o], const, ok)

    def cnt(self):
        return np.diff(self.ptr)

    def rows(self):
        return np.repeat(np.arange(self.n), self.cnt())

    def take(self, idx):
        idx = np.asarray(idx, np.int64)
        c = self.cnt()[idx]
        ptr = np.concatenate([[0], np.cumsum(c)]).astype(np.int64)
        tot = int(ptr[-1])
        if tot:
            pos = np.repeat(self.ptr[idx] - ptr[:-1], c) + np.arange(tot)
            leaf, coef = self.leaf[pos], self.coef[pos]
        else:
            leaf, coef = np.zeros(0, U), np.zeros(0)
        return AF(idx.size, ptr, leaf, coef, self.const[idx], self.ok[idx])

    def bc(self, L):
        if self.n == L:
            return self
        return self.take(np.zeros(L, np.int64))

    def scale(self, v):
        v = np.broadcast_to(np.asarray(v, np.float64), (self.n,))
        return AF(self.n, self.ptr, self.leaf, self.coef * np.repeat(v, self.cnt()),
                  self.const * v, self.ok)

    def add(self, o, sign=1.0):
        return AF.coo(self.n, np.concatenate([self.rows(), o.rows()]),
                      np.concatenate([self.leaf, o.leaf]),
                      np.concatenate([self.coef, sign * o.coef]),
                      self.const + sign * o.const, self.ok & o.ok)

    def put(self, idx, o):
        keep = np.ones(self.n, bool)
        keep[idx] = False
        rows = self.rows()
        m = keep[rows]
        const, ok = self.const.copy(), self.ok.copy()
        const[idx], ok[idx] = o.const, o.ok
        return AF.coo(self.n, np.concatenate([rows[m], np.asarray(idx)[o.rows()]]),
                      np.concatenate([self.leaf[m], o.leaf]),
                      np.concatenate([self.coef[m], o.coef]), const, ok)

    def pure_data(self):
        return self.ptr[-1] == 0 and bool(self.ok.all())

    def rowhash(self):
        """Key of each row's leaf part (constant ignored)."""
        h = np.full(self.n, 31, dtype=U)
        if self.leaf.size:
            e = mix(self.leaf, self.coef.view(U))
            nz = self.cnt() > 0
            red = np.bitwise_xor.reduceat(e, self.ptr[:-1][nz])
            h[nz] = mix(U(37), red)
        return h


def family_of(fam):
    if fam.startswith("NORMAL_") or fam == "STD_NORMAL_LPDF":
        return "normal"
    if fam.startswith(("BERNOULLI", "BINOMIAL")):
        return "bern/binom"
    if fam.startswith("POISSON") and fam.endswith("LPMF"):
        return "poisson"
    if fam.startswith("MULTI_NORMAL"):
        return "multi_normal"
    if fam.startswith("MULTINOMIAL"):
        return "multinomial"
    if fam.startswith("NEG_BINOMIAL_2"):
        return "neg_binomial_2"
    for suf in ("_LPDF", "_LPMF"):
        if fam.endswith(suf):
            return fam[:-len(suf)].lower()
    return fam.lower()


class Analysis2(A1.Analysis):
    def pre(self):
        self.A = {}
        self.LA = {}          # slot -> AF of log(value), for exp(affine)
        self.leaf_slots = set()

    def af(self, s):
        if s not in self.A:
            ln, is_param, _a = self.slots[s]
            if is_param:
                self.A[s] = AF.leaves(self.get(s)[0], np.ones(ln, bool))
                self.leaf_slots.add(s)
            elif s in self.vals:
                self.A[s] = AF.data(self.vals[s])
            else:
                self.A[s] = AF.unknown(ln)
        return self.A[s]

    def set_leaves(self, s):
        k, _sk, f = self.get(s)
        self.A[s] = AF.leaves(k, (f & ACTIVE) != 0)
        self.leaf_slots.add(s)

    def matvec_af(self, X, beta, rows, cols):
        Xv, b = self.af(X), self.af(beta)
        if not Xv.pure_data() or Xv.n != rows * cols or b.n != cols:
            return None
        Xm = Xv.const.reshape(cols, rows)
        rr, ll, cc = [], [], []
        bc = b.cnt()
        for c in range(cols):
            if not bc[c]:
                continue
            nz = np.nonzero(Xm[c])[0]
            for e in range(b.ptr[c], b.ptr[c + 1]):
                rr.append(nz)
                ll.append(np.full(nz.size, b.leaf[e], U))
                cc.append(Xm[c][nz] * b.coef[e])
        cat = (lambda x, dt: np.concatenate(x) if x else np.zeros(0, dt))
        return AF.coo(rows, cat(rr, np.int64), cat(ll, U), cat(cc, np.float64),
                      Xm.T @ b.const, np.full(rows, bool(b.ok.all())))

    def post(self, op):
        n = op.name
        slots = self.slots
        L = slots[op.out][0] if op.out >= 0 else 0
        out = None
        la = None
        try:
            if n == "OP_INDEX":
                i = [int(op.idata[0])]
                out = self.af(op.ins[0]).take(i)
                if op.ins[0] in self.LA:
                    la = self.LA[op.ins[0]].take(i)
            elif n == "OP_GATHER":
                out = self.af(op.ins[0]).take(op.idata)
                if op.ins[0] in self.LA:
                    la = self.LA[op.ins[0]].take(op.idata)
            elif n == "OP_SLICE":
                ix = int(op.idata[0]) + np.arange(L)
                out = self.af(op.ins[0]).take(ix)
                if op.ins[0] in self.LA:
                    la = self.LA[op.ins[0]].take(ix)
            elif n == "OP_SLICE_STRIDED":
                out = self.af(op.ins[0]).take(int(op.idata[0]) + np.arange(L) * int(op.idata[1]))
            elif n.startswith("OP_SET_") and len(op.ins) == 2 and "DYNAMIC" not in n:
                v = self.af(op.ins[1])
                out = self.af(op.ins[0]).put(self.write_index(op, v.n), v)
            elif n == "OP_CONCAT2":
                a, b = self.af(op.ins[0]), self.af(op.ins[1])
                out = AF.coo(a.n + b.n, np.concatenate([a.rows(), a.n + b.rows()]),
                             np.concatenate([a.leaf, b.leaf]),
                             np.concatenate([a.coef, b.coef]),
                             np.concatenate([a.const, b.const]),
                             np.concatenate([a.ok, b.ok]))
            elif n == "OP_REP_VEC" and len(op.ins) == 1:
                out = self.af(op.ins[0]).take(np.zeros(L, np.int64))
            elif n == "OP_TRANSPOSE":
                r, c = int(op.idata[0]), int(op.idata[1])
                out = self.af(op.ins[0]).take(np.arange(r * c).reshape(c, r).T.reshape(-1))
            elif n in ("OP_ADD", "OP_SUB") and all(slots[s][0] in (1, L) for s in op.ins):
                out = self.af(op.ins[0]).bc(L).add(self.af(op.ins[1]).bc(L),
                                                   1.0 if n == "OP_ADD" else -1.0)
            elif n == "OP_NEG":
                out = self.af(op.ins[0]).scale(-1.0)
            elif n in ("OP_MUL", "OP_DIV") and all(slots[s][0] in (1, L) for s in op.ins):
                a, b = self.af(op.ins[0]).bc(L), self.af(op.ins[1]).bc(L)
                if b.pure_data():
                    out = a.scale(b.const if n == "OP_MUL" else 1.0 / b.const)
                    src, d = op.ins[0], b.const
                elif a.pure_data() and n == "OP_MUL":
                    out = b.scale(a.const)
                    src, d = op.ins[1], a.const
                else:
                    src = None
                if src is not None and src in self.LA and n == "OP_MUL" and (d > 0).all():
                    q = self.LA[src].bc(L)
                    la = AF(q.n, q.ptr, q.leaf, q.coef, q.const + np.log(d), q.ok)
            elif n in ("OP_FMA", "OP_BCAST_FMA") and len(op.ins) == 3:
                a, b, x = (self.af(s).bc(L) for s in op.ins)
                if b.pure_data():
                    out = a.add(x.scale(b.const))
                elif x.pure_data():
                    out = a.add(b.scale(x.const))
            elif n == "OP_MATVEC":
                out = self.matvec_af(op.ins[0], op.ins[1], int(op.idata[0]), int(op.idata[1]))
            elif n == "OP_DOT" and op.variant != 1 and len(op.ins) == 2:
                a, b = self.af(op.ins[0]), self.af(op.ins[1])
                if b.pure_data() and a.n == b.n:
                    a, b = b, a
                if a.pure_data() and a.n == b.n:
                    v = b.scale(a.const)
                    out = AF.coo(1, np.zeros(v.leaf.size, np.int64), v.leaf, v.coef,
                                 np.array([v.const.sum()]), np.array([bool(v.ok.all())]))
            elif n in ("OP_EXPV", "OP_EXP") and len(op.ins) == 1:
                la = self.af(op.ins[0])
        except Exception:  # noqa: BLE001  (fall back to leaves)
            out = None
        if op.out >= 0:
            if out is not None and out.n == L:
                self.A[op.out] = out
            else:
                self.set_leaves(op.out)
            if la is not None and la.n == L:
                self.LA[op.out] = la
            else:
                self.LA.pop(op.out, None)
        if op.out2 >= 0:
            self.set_leaves(op.out2)
        if self.dens and self.dens[-1]["op"] == op.i:
            self.annotate(op, self.dens[-1])

    # ---- extra density annotations ------------------------------------
    def annotate(self, op, rec):
        n, fam = op.name, rec["fam"]
        slots = self.slots
        rec["family"] = family_of(fam)
        inactive = lambda s: not slots[s][2]  # noqa: E731
        if rec["kind"] == "generic":
            name_h = shash(n)
            if n == "OP_CATEGORICAL" and inactive(op.ins[0]):
                rec["family"] = "categorical"
                rec["grp"] = mix(name_h, vechash(self.K[op.ins[1]]))[None].astype(U)
                rec["elig"] = True
            elif fam.startswith("MULTINOMIAL") and len(op.ins) == 1:
                rec["grp"] = mix(name_h, vechash(self.K[op.ins[0]]))[None].astype(U)
                rec["elig"] = True
            elif fam == "DIRICHLET_LPDF" and inactive(op.ins[0]):
                rec["grp"] = mix(name_h, vechash(self.K[op.ins[1]]))[None].astype(U)
                rec["elig"] = True
            elif fam == "DIRICHLET_LPDF":
                rec["variate_active"] = np.ones(1, bool)
            elif fam.startswith("MULTI_NORMAL") and len(op.ins) == 3 and op.idata.size == 3:
                K, Ny, Nmu = (int(x) for x in op.idata)
                y, mu, Sg = op.ins
                if not inactive(y):
                    rec["variate_active"] = np.ones(1, bool)
                    return
                sg = vechash(self.K[Sg])
                N = max(Ny, 1)
                ky = self.K[y].reshape(N, K)
                yh = np.full(N, 41, dtype=U)
                for c in range(K):
                    yh = mix(yh, ky[:, c])
                if Nmu > 1 and self.K[mu].size == N * K:
                    km = self.K[mu].reshape(N, K)
                    mh = np.full(N, 43, dtype=U)
                    for c in range(K):
                        mh = mix(mh, km[:, c])
                else:
                    mh = np.full(N, vechash(self.K[mu]), dtype=U)
                grp = mix(mix(np.full(N, name_h, U), mh), sg)
                rec.update(N=N, grp=grp, full=mix(grp, yh), gsk=grp.copy(),
                           kind="mvn", elig=True, mvn_K=K,
                           variate_active=np.zeros(N, bool))
            else:
                # lkj, wishart, wiener, gp, ...: not a data-variate
                # exponential-family term this census can collapse
                rec["variate_active"] = np.ones(1, bool) if not (
                    op.ins and inactive(op.ins[0])) else np.zeros(1, bool)
            return
        if rec["kind"] == "glm":
            if fam == "NORMAL_ID_GLM_LPDF":
                y, X, alpha, beta, sigma = op.ins[:5]
                mv = self.matvec_af(X, beta, int(op.idata[0]), int(op.idata[1]))
                if mv is not None and inactive(y):
                    N = rec["N"]
                    rec["lg"] = dict(y=np.broadcast_to(self.af(y).const, (N,)),
                                     mean=self.af(alpha).bc(N).add(mv),
                                     sig=np.broadcast_to(self.K[sigma], (N,)), log=False)
            return
        N = rec["N"]
        if fam in ("NORMAL_LPDF", "LOGNORMAL_LPDF") and len(op.ins) == 3 \
                and inactive(op.ins[0]):
            rec["lg"] = dict(y=np.broadcast_to(self.af(op.ins[0]).const, (N,)),
                             mean=self.af(op.ins[1]).bc(N),
                             sig=np.broadcast_to(self.K[op.ins[2]], (N,)),
                             log=fam == "LOGNORMAL_LPDF")
        if fam in ("POISSON_LOG_LPMF", "POISSON_LPMF") and len(op.ins) == 1:
            eta = self.af(op.ins[0]) if fam == "POISSON_LOG_LPMF" else self.LA.get(op.ins[0])
            if eta is not None:
                eta = eta.bc(N)
                rec["grp_off"] = np.where(eta.ok & (eta.cnt() > 0),
                                          mix(shash("pois_off"), eta.rowhash()),
                                          rec["grp"])
        if fam == "WEIBULL_LPDF" and inactive(op.ins[0]) and inactive(op.ins[1]):
            rec["elig"] = True
        if fam.startswith("NEG_BINOMIAL_2") and fam.endswith("LPMF") \
                and len(op.ins) == 2 and inactive(op.ins[1]):
            rec["elig"] = True

    # ---- preparation: eligibility and linear-Gaussian groups -----------
    def prepare(self):
        super().prepare()
        for d in self.dens:
            d.setdefault("family", family_of(d["fam"]))
            d["variate_data"] = not bool(np.asarray(d["variate_active"]).any())
            d["eligible_a"] = d["variate_data"] and (d["fam"] in EXPFAM or d.get("elig", False))
        self.lg_groups = []
        by_sig = collections.defaultdict(list)
        for d in self.dens:
            lg = d.get("lg")
            if lg is None or not d["lin"] or not d["variate_data"]:
                continue
            m = lg["mean"]
            if not m.ok.all() or np.unique(lg["sig"]).size != 1:
                continue
            by_sig[(int(lg["sig"][0]), lg["log"])].append(d)
        for (_sig, is_log), ds in by_sig.items():
            N = sum(d["N"] for d in ds)
            if N < 2:
                continue
            rows, leaf, coef, const, off = [], [], [], [], 0
            for d in ds:
                m = d["lg"]["mean"]
                rows.append(off + m.rows())
                leaf.append(m.leaf)
                coef.append(m.coef)
                const.append(m.const)
                off += d["N"]
            rows, leaf, coef = (np.concatenate(x) for x in (rows, leaf, coef))
            if leaf.size == 0:
                continue
            ukeys, col = np.unique(leaf, return_inverse=True)
            P = ukeys.size
            # merge duplicate (row, col) entries
            rc, inv = np.unique(rows.astype(np.int64) * P + col, return_inverse=True)
            val = np.zeros(rc.size)
            np.add.at(val, inv, coef)
            keep = val != 0
            rc, val = rc[keep], val[keep]
            r, c = rc // P, rc % P
            colnnz = np.bincount(c, minlength=P)
            rownnz = np.bincount(r, minlength=N)
            dense = colnnz >= 0.5 * N
            # distinct rows of [Z | const]: groups of the weighted (a) form
            rh = np.zeros(N, dtype=U)
            np.bitwise_xor.at(rh, r, mix(mix(U(47), c.astype(U)), val.view(U)))
            rh = mix(rh, np.concatenate(const).view(U))
            gram_nnz = None
            if N * P <= 2e8:
                B = np.zeros((N, P), np.float32)
                B[r, c] = 1.0
                gram_nnz = int(np.count_nonzero(B.T @ B))
            elif int((rownnz.astype(np.int64) ** 2).sum()) <= 5e7:
                pairs = []
                order = np.argsort(r, kind="stable")
                cs, ptr = c[order], np.concatenate([[0], np.cumsum(rownnz)])
                for i in range(N):
                    x = cs[ptr[i]:ptr[i + 1]]
                    pairs.append((x[:, None] * P + x[None, :]).ravel())
                gram_nnz = int(np.unique(np.concatenate(pairs)).size)
            g = dict(N=int(N), P=int(P), n_ops=len(ds), nnzZ=int(val.size),
                     K_dense=int(dense.sum()), G_sparse=int((~dense).sum()),
                     sparse_all_unit=bool((val[~dense[c]] == 1.0).all()),
                     max_row_nnz=int(rownnz.max()), distinct_rows=int(np.unique(rh).size),
                     gram_nnz=gram_nnz, lognormal=bool(is_log),
                     ops=sorted({d["name"][3:] for d in ds}),
                     pure_indicator=bool(rownnz.max() == 1 and (val == 1.0).all()))
            g["new_ns"] = NS_FIXED + NS_PER_NNZ * (gram_nnz if gram_nnz is not None else P * P)
            g["_leaves"] = ukeys
            g["_Z"] = (r, c, val)
            g["_const"] = np.concatenate(const)
            g["_y"] = np.concatenate([np.asarray(d["lg"]["y"], float) for d in ds])
            g["_ops"] = {d["op"] for d in ds}
            self.lg_groups.append(g)
            for d in ds:
                d["lg_ok"] = True
                d["lg_gid"] = len(self.lg_groups) - 1

    # ---- demand hooks ---------------------------------------------------
    def seed(self, dm, cfg):
        if not isinstance(cfg, dict) or "lg" not in cfg["fams"]:
            return
        need = [g["_leaves"] for g in self.lg_groups if not g["lognormal"]]
        if not need:
            return
        need = np.unique(np.concatenate(need))
        for s in self.leaf_slots:
            hit = np.isin(self.Kfinal[s], need)
            if hit.any():
                dm(s)[hit] = True

    def dens_keys(self, rec, cfg):
        if not isinstance(cfg, dict):
            return super().dens_keys(rec, cfg)
        fams = cfg["fams"]
        if "lg" in fams and rec.get("lg_ok") and not rec["lg"]["log"]:
            return "drop"
        if rec["eligible_a"]:
            if rec["family"] == "poisson" and "poisson+offset" in fams and "grp_off" in rec:
                return rec["grp_off"]
            if rec["family"] in fams:
                return rec["grp"]
        return rec["full"] if "b" in fams else None

    def front_keys(self, s, opname, size, cfg):
        if not isinstance(cfg, dict):
            return super().front_keys(s, opname, size, cfg)
        dd = self.out_dens.get(s)
        if dd is not None and dd["outlen"] == dd["N"]:
            k = self.dens_keys(dd, cfg)
            if k is not None:
                return k
        return self.Kfinal[s] if "b" in cfg["fams"] else None


def analyze2(name, prof):
    A = Analysis2(HERE / "out" / name / "census.txt.gz")
    A.prepare()
    w = A.weights()
    t = np.zeros(len(A.ops))
    covered = 0.0
    byname = collections.defaultdict(list)
    for op in A.ops:
        byname[op.name].append(op.i)
    for oc, p in (prof or {}).items():
        ids = byname.get(oc)
        if not ids:
            continue
        ww = w[ids]
        tt = p["fwd"] + p["bwd"]
        t[ids] = tt * ww / max(ww.sum(), 1e-300)
        covered += tt
    total = sum(p["fwd"] + p["bwd"] for p in (prof or {}).values())
    res = {"name": name, "prof_total": total}
    # relevant families for this model
    rel = set()
    for d in A.dens:
        if d["eligible_a"] and d["N"] >= 1:
            rel.add(d["family"])
        if "grp_off" in d and d["eligible_a"] and (d["grp_off"] != d["grp"]).any():
            rel.add("poisson+offset")
    if any(not g["lognormal"] for g in A.lg_groups):
        rel.add("lg")
    rel.add("b")
    base = A.demand({"fams": frozenset()})
    tb = float((t * base).sum()) + (total - covered)
    shares = {}
    rel = sorted(rel)
    subsets = [frozenset(c) for k in range(1, len(rel) + 1)
               for c in itertools.combinations(rel, k)] if len(rel) <= 7 else \
        [frozenset([f]) for f in rel] + [frozenset(rel)]
    for S in subsets:
        fr = A.demand({"fams": S})
        tn = float((t * fr).sum()) + (total - covered)
        extra = sum(g["new_ns"] for g in A.lg_groups if not g["lognormal"]) if "lg" in S else 0.0
        shares["|".join(sorted(S))] = dict(saved=(1 - tn / tb) if tb > 0 else 0.0,
                                           extra_ns=extra)
    res["relevant"] = rel
    res["subsets"] = shares
    res["lg"] = [{k: v for k, v in g.items() if not k.startswith("_")} for g in A.lg_groups]
    # per-term share of profile time for linear-Gaussian groups
    for g, o in zip(A.lg_groups, res["lg"]):
        o["dens_time_share"] = float(sum(t[i] for i in g["_ops"])) / total if total else None
    groups = collections.defaultdict(list)
    for d in A.dens:
        groups[(d["name"], d["variate_data"], d["lin"], d["kind"], d["eligible_a"])].append(d)
    dens_elems = sum(d["N"] for d in A.dens)
    terms = []
    for (opn, vdata, lin, kind, elig), ds in groups.items():
        N = sum(d["N"] for d in ds)
        cat = lambda k: np.concatenate([np.asarray(d[k]).reshape(-1) for d in ds])  # noqa: E731
        cls = A1.classify([d["cols"] for d in ds], ds[0]["n_var"]) if kind == "scalar" else [kind]
        tm = dict(op=opn[3:], family=ds[0]["family"], n_ops=len(ds), N=int(N),
                  variate_data=bool(vdata), lin=bool(lin), kind=kind, eligible=bool(elig),
                  classes=cls, G_grp=int(np.unique(cat("grp")).size),
                  G_b=int(np.unique(cat("full")).size),
                  elem_share=N / max(dens_elems, 1),
                  time_share=float(sum(t[d["op"]] for d in ds)) / total if total else None)
        if all("grp_off" in d for d in ds):
            tm["G_off"] = int(np.unique(cat("grp_off")).size)
        if kind == "mvn":
            tm["mvn_K"] = ds[0]["mvn_K"]
        terms.append(tm)
    res["terms"] = terms
    return res


def run(args):
    name, prof = args
    try:
        return analyze2(name, prof)
    except Exception as e:  # noqa: BLE001
        import traceback
        return {"name": name, "error": f"{type(e).__name__}: {e}",
                "trace": traceback.format_exc()[-1500:]}


def main():
    profs = {r["name"]: r for r in json.loads((HERE / "stage2_profiles.json").read_text())
             if r.get("status") == "ok"}
    names = sys.argv[1:] or sorted(p.name for p in (HERE / "out").iterdir()
                                   if (p / "census.txt.gz").exists())
    jobs = [(n, profs.get(n, {}).get("census_prof")) for n in names]
    out = []
    if len(names) <= 4:
        out = [run(j) for j in jobs]
    else:
        with concurrent.futures.ProcessPoolExecutor(8) as pool:
            for r in pool.map(run, jobs, chunksize=2):
                out.append(r)
                print(r["name"], r.get("error", "ok"), file=sys.stderr, flush=True)
    for r in out:
        if r["name"] in profs:
            r["census_ns"] = profs[r["name"]]["census_ns"]
    if sys.argv[1:]:
        print(json.dumps(out, indent=1))
    else:
        (HERE / "analysis2.json").write_text(json.dumps(out))


if __name__ == "__main__":
    main()
