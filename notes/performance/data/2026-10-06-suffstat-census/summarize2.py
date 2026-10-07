#!/usr/bin/env python3
"""Summarize analysis2.json: linear-Gaussian form, families, ranked list.

All ceilings are ESTIMATES:  T(S) = min(default_ns, census_ns * (1 -
saved_share(S)) + extra_ns(S)),  ceiling = default_ns / T(S).  extra_ns is
the assumed cost of the quadratic form, 100 ns + 2 ns per nonzero of Z'Z.
"""
import collections
import json
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
rows = [r for r in json.loads((HERE / "analysis2.json").read_text()) if "error" not in r]
fast = {r["name"]: r for r in json.loads((HERE / "stage2b_fast.json").read_text())
        if r.get("status") == "ok"}
v1 = {r["name"]: r for r in json.loads((HERE / "analysis_with_estimates_v1.json").read_text())}
out = []
P = out.append


def T(r, S):
    d = fast[r["name"]]["default_ns"]
    S = frozenset(S) & frozenset(r["relevant"])
    if not S:
        return d
    e = r["subsets"]["|".join(sorted(S))]
    return min(d, r["census_ns"] * (1 - e["saved"]) + e["extra_ns"])


def ceil(r, S, above=()):
    return T(r, above) / T(r, set(S) | set(above))


timed = [r for r in rows if r["name"] in fast and r.get("prof_total")]

# the 36 flagged in the first report (same rule, recomputed from v1)
gram36 = set()
for name, r in v1.items():
    for t in r["terms"]:
        if not (t["variate_data"] and t["N"] >= 8 and t["elem_share"] >= 0.1):
            continue
        best = min(t["G_grp"], t["G_b"]) if (t["expfam"] and t["lin"]) else t["G_b"]
        if best <= t["N"] / 2:
            continue
        cls = [c for c in t["classes"] if not c.startswith("y:")]
        if t["op"] == "NORMAL_ID_GLM_LPDF" or (t["op"] == "NORMAL_LPDF" and cls
                                               and cls[0] == "linpred" and cls[-1] == "bcast"):
            gram36.add(name)

P("== 1. linear-Gaussian (Gram / QR) form ==")
P(f"models flagged in the first report: {len(gram36)}")
lgm = {}
for r in timed:
    gs = [g for g in r["lg"] if not g["lognormal"] and g["N"] >= 8]
    if gs:
        lgm[r["name"]] = (r, max(gs, key=lambda g: g["N"]))
P(f"models with a linear-Gaussian term (data variate, one broadcast sigma, affine "
  f"mean, N>=8): {len(lgm)}")
hdr = (f"{'model':34s} {'N':>6s} {'P':>5s} {'Kden':>4s} {'Gsp':>5s} {'nnzZ':>8s} "
       f"{'nnzZtZ':>8s} {'P^2':>8s} {'rows':>6s} {'ceil':>7s} {'net(a)':>6s} "
       f"{'net(a,b)':>8s} {'ns/grad':>9s}  note")


def line(name):
    r, g = lgm[name]
    c = ceil(r, {"lg"})
    net = ceil(r, {"lg"}, {"normal"})
    net2 = ceil(r, {"lg"}, {"normal", "b"})
    note = []
    if g["pure_indicator"]:
        note.append("pure indicator = grouped normal")
    if g["P"] ** 2 >= g["nnzZ"]:
        note.append("P^2>=nnz(Z)")
    if g["gram_nnz"] is not None and g["gram_nnz"] >= g["nnzZ"]:
        note.append("nnz(Z'Z)>=nnz(Z)")
    return (f"{name[:34]:34s} {g['N']:6d} {g['P']:5d} {g['K_dense']:4d} {g['G_sparse']:5d} "
            f"{g['nnzZ']:8d} {str(g['gram_nnz']):>8s} {g['P'] ** 2:8d} "
            f"{g['distinct_rows']:6d} {c:7.2f} {net:6.2f} {net2:8.2f} "
            f"{fast[name]['default_ns']:9.0f}  {'; '.join(note)}")


in36 = sorted((n for n in gram36 if n in lgm), key=lambda n: -ceil(lgm[n][0], {"lg"}))
P(f"of the {len(gram36)} flagged: qualify {len(in36)}; not qualifying: "
  f"{' '.join(sorted(gram36 - set(lgm)))}")
c36 = [ceil(lgm[n][0], {"lg"}) for n in in36]
P(f"  estimated ceiling >=2x: {sum(c >= 2 for c in c36)}; >=10x: {sum(c >= 10 for c in c36)}; "
  f"<1.1x: {sum(c < 1.1 for c in c36)}")
P(f"  dense P^2 >= nnz(Z) [no win without structure]: "
  f"{sum(lgm[n][1]['P'] ** 2 >= lgm[n][1]['nnzZ'] for n in in36)}; structured nnz(Z'Z) >= nnz(Z): "
  f"{sum((lgm[n][1]['gram_nnz'] or 10**18) >= lgm[n][1]['nnzZ'] for n in in36)}")
P("columns: Kden/Gsp = columns of Z with >= / < N/2 nonzeros; rows = distinct rows of "
  "[Z|c]; ceil = est. ceiling vs default; net(a) = net of grouped normal; "
  "net(a,b) = net of grouped normal and duplicate rows")
P(hdr)
for n in in36:
    P(line(n))
others = sorted((n for n in lgm if n not in gram36), key=lambda n: -ceil(lgm[n][0], {"lg"}))
pure = [n for n in others if lgm[n][1]["pure_indicator"]]
P("")
P(f"beyond the flagged set: {len(others)} more models qualify; {len(pure)} are pure "
  f"indicator designs (identical to grouped normal); the rest:")
P(hdr)
for n in others:
    if n not in pure:
        P(line(n))
allc = {n: ceil(lgm[n][0], {"lg"}) for n in lgm}
netc = {n: ceil(lgm[n][0], {"lg"}, {"normal"}) for n in lgm}
P(f"all {len(lgm)} qualifying models: ceiling >=2x {sum(c >= 2 for c in allc.values())}, "
  f">=10x {sum(c >= 10 for c in allc.values())}; net of grouped normal >=2x "
  f"{sum(c >= 2 for c in netc.values())}, >=10x {sum(c >= 10 for c in netc.values())}")
big = {n: c for n, c in allc.items() if lgm[n][1]["N"] >= 200}
P(f"  of those with N>=200 ({len(big)} models): >=2x {sum(c >= 2 for c in big.values())}, "
  f">=10x {sum(c >= 10 for c in big.values())}; N<200: >=2x "
  f"{sum(c >= 2 for n, c in allc.items() if n not in big)} (fixed-overhead wins: the "
  f"term becomes one op)")
P(f"  dense P^2 >= nnz(Z): {sum(g['P'] ** 2 >= g['nnzZ'] for _r, g in lgm.values())} of "
  f"{len(lgm)}; structured nnz(Z'Z) >= nnz(Z): "
  f"{sum((g['gram_nnz'] or 10**18) >= g['nnzZ'] for _r, g in lgm.values())}; of the "
  f"models at >=2x, dense P^2 >= nnz(Z) (need the sparse/arrow structure): "
  f"{sum(lgm[n][1]['P'] ** 2 >= lgm[n][1]['nnzZ'] for n, c in allc.items() if c >= 2)}")
lgn = [r["name"] for r in timed if any(g["lognormal"] and g["N"] >= 8 for g in r["lg"])]
P(f"lognormal with affine log-mean (same form on log y): {len(lgn)} models: {' '.join(lgn)}")

P("")
P("== 2. families: data-variate density terms by family ==")
P("present = models with a data-variate term (N>=2) of the family; eligible = its "
  "non-variate arguments admit a finite statistic; collapses = eligible, summed "
  "linearly into target, groups < N; then models at estimated standalone ceiling")
fam = collections.defaultdict(lambda: collections.defaultdict(set))
detail = collections.defaultdict(list)
for r in rows:
    for t in r["terms"]:
        if not t["variate_data"] or t["N"] < 2:
            continue
        f = t["family"]
        fam[f]["present"].add(r["name"])
        if t["eligible"]:
            fam[f]["eligible"].add(r["name"])
        if t["eligible"] and t["lin"] and t["G_grp"] < t["N"]:
            fam[f]["collapses"].add(r["name"])
            if t["G_grp"] <= t["N"] / 2:
                fam[f]["2x groups"].add(r["name"])
            detail[f].append((r["name"], t["op"], t["N"], t["G_grp"], t["classes"]))
        if t["family"] == "poisson" and t.get("G_off", t["G_grp"]) < t["G_grp"]:
            fam["poisson+offset"]["present"].add(r["name"])
            fam["poisson+offset"]["eligible"].add(r["name"])
            fam["poisson+offset"]["collapses"].add(r["name"])
            detail["poisson+offset"].append((r["name"], t["op"], t["N"], t["G_off"],
                                             t["classes"]))
ALL = ["normal", "bern/binom", "poisson", "poisson+offset", "lognormal", "gamma", "beta",
       "exponential", "inv_gamma", "weibull", "von_mises", "dirichlet", "multinomial",
       "categorical", "multi_normal", "neg_binomial_2", "neg_binomial", "beta_binomial",
       "student_t", "cauchy", "ordered_logistic", "chi_square", "inv_chi_square",
       "scaled_inv_chi_square", "rayleigh", "beta_proportion", "pareto"]
for f in sorted(fam):
    if f not in ALL:
        ALL.append(f)
P(f"{'family':24s} {'present':>7s} {'eligible':>8s} {'collapses':>9s} {'>=2x grp':>8s} "
  f"{'ceil>=2x':>8s} {'ceil>=10x':>9s}")
for f in ALL:
    c = [ceil(r, {f}) for r in timed if f in r["relevant"]]
    P(f"{f:24s} {len(fam[f]['present']):7d} {len(fam[f]['eligible']):8d} "
      f"{len(fam[f]['collapses']):9d} {len(fam[f]['2x groups']):8d} "
      f"{sum(x >= 2 for x in c):8d} {sum(x >= 10 for x in c):9d}")
P("")
P("collapsing terms outside normal / bern-binom / poisson (model, op, N, groups, args):")
for f in ALL:
    if f in ("normal", "bern/binom", "poisson"):
        continue
    for name, op, N, G, cls in sorted(detail[f]):
        rr = [r for r in timed if r["name"] == name]
        c = ceil(rr[0], {f}) if rr and f in rr[0]["relevant"] else float("nan")
        P(f"  {f:16s} {name:28s} {op:28s} N={N:6d} groups={G:6d} ceil={c:6.2f} {','.join(cls)}")

P("")
P("every data-variate term of the remaining families (model, op, terms N, groups, "
  "eligible, linear-to-target, args):")
for r in rows:
    for t in r["terms"]:
        if t["variate_data"] and t["N"] >= 2 and t["family"] not in (
                "normal", "bern/binom", "poisson"):
            extra = f" K={t['mvn_K']}" if "mvn_K" in t else ""
            P(f"  {t['family']:18s} {r['name']:28s} {t['op']:26s} N={t['N']:6d} "
              f"groups={t['G_grp']:6d} tuples={t['G_b']:6d} elig={int(t['eligible'])} "
              f"lin={int(t['lin'])}{extra} {','.join(t['classes'])}")
P("")
P("== 3. ranked list (greedy; each row net of the rows above) ==")
cands = ["normal", "bern/binom", "poisson", "lg", "b"] + [
    f for f in ALL if f not in ("normal", "bern/binom", "poisson")
    and any(f in r["relevant"] for r in timed)]
chosen = []
P(f"{'rank':4s} {'family':22s} {'alone>=2x':>9s} {'alone>=10x':>10s} {'net>=1.1x':>9s} "
  f"{'net>=2x':>8s} {'net>=10x':>9s} {'cum>=2x':>8s} {'cum>=10x':>9s}")
while cands:
    score = {}
    for f in cands:
        net = [ceil(r, {f}, chosen) for r in timed]
        score[f] = (sum(x >= 2 for x in net), sum(x >= 10 for x in net),
                    sum(x >= 1.1 for x in net))
    f = max(cands, key=lambda k: score[k])
    alone = [ceil(r, {f}) for r in timed]
    chosen.append(f)
    cands.remove(f)
    cum = [ceil(r, chosen) for r in timed]
    P(f"{len(chosen):4d} {f:22s} {sum(x >= 2 for x in alone):9d} "
      f"{sum(x >= 10 for x in alone):10d} {score[f][2]:9d} {score[f][0]:8d} "
      f"{score[f][1]:9d} {sum(x >= 2 for x in cum):8d} {sum(x >= 10 for x in cum):9d}")
P("")
P("fixed build order (simplest first), same columns:")
chosen = []
for f in ["normal", "bern/binom", "poisson", "b", "lg", "lognormal"]:
    net = [ceil(r, {f}, chosen) for r in timed]
    alone = [ceil(r, {f}) for r in timed]
    chosen.append(f)
    cum = [ceil(r, chosen) for r in timed]
    P(f"{len(chosen):4d} {f:22s} {sum(x >= 2 for x in alone):9d} "
      f"{sum(x >= 10 for x in alone):10d} {sum(x >= 1.1 for x in net):9d} "
      f"{sum(x >= 2 for x in net):8d} {sum(x >= 10 for x in net):9d} "
      f"{sum(x >= 2 for x in cum):8d} {sum(x >= 10 for x in cum):9d}")
P(f"(of {len(timed)} timed models; 'lg' = linear-Gaussian form, 'b' = duplicate-row weighting)")
(HERE / "summary2.txt").write_text("\n".join(out) + "\n")
print("\n".join(out))
