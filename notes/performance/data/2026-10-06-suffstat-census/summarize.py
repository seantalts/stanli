#!/usr/bin/env python3
"""Summarize analysis.json: corpus counts, top models, near-miss buckets.

Ceilings are ESTIMATES: est_collapsed_ns = census_ns * (1 - saved_share),
where saved_share is the share of the census-configuration opcode profile
attributed to elements that would no longer be computed, and the ceiling is
measured ns/gradient (shipped default, or runtime fast-math) divided by it.
"""
import collections
import json
import pathlib
import re

HERE = pathlib.Path(__file__).resolve().parent
rows = json.loads((HERE / "analysis.json").read_text())
fast = {r["name"]: r for r in json.loads((HERE / "stage2b_fast.json").read_text())
        if r.get("status") == "ok"}
MIN_N = 8


def src(name):
    return (HERE / "out" / name / "model.stan").read_text()


def block(text, which):
    m = re.search(which + r"\s*\{", text)
    return text[m.end():] if m else ""


def a_ok(t):
    return t["variate_data"] and t["expfam"] and t["lin"] and t["G_grp"] < t["N"]


def strict(t):
    return t["kind"] == "scalar" and not any(
        c in ("covar", "linpred") for c in t["classes"])


out = []
P = out.append
ok = [r for r in rows if "error" not in r]
P(f"models analysed: {len(ok)} of {len(rows)}; with profile: "
  f"{sum(1 for r in ok if r.get('prof_total'))}; with default/fast timing: "
  f"{sum(1 for r in ok if r['name'] in fast)}")

H = collections.defaultdict(dict)
buckets = collections.defaultdict(list)
gram = set()
fam_a = collections.defaultdict(set)
fam_b = collections.defaultdict(set)
for r in ok:
    lik = [t for t in r["terms"] if t["variate_data"] and t["N"] >= 2]
    big = [t for t in lik if t["N"] >= MIN_N]
    for key, sel in (("a_strict", lambda t: a_ok(t) and strict(t)),
                     ("a_repeated_covariate", lambda t: a_ok(t) and not strict(t)),
                     ("a_any", a_ok),
                     ("b", lambda t: t["G_b"] < t["N"])):
        tt = [t for t in lik if sel(t)]
        if tt:
            H[key][r["name"]] = max(
                t["N"] / (t["G_b"] if key == "b" else t["G_grp"]) for t in tt)
    f = r.get("frontier", {}).get("b", [0, 0])
    r["front_b"] = f
    if f[0] - f[1] >= 1 and r["name"] not in H["b"]:
        H["b_frontier_only"][r["name"]] = f[0] / max(f[1], 1)
    s = src(r["name"])
    r["has_loglik_gq"] = bool(re.search(r"log_lik", block(s, "generated quantities")))
    r["has_trunc"] = bool(re.search(r"\)\s*T\[", s))
    r["has_cdf"] = any(re.search(r"CDF$", t["op"]) for t in r["terms"])
    fr = fast.get(r["name"])
    if fr and r.get("prof_total"):
        r["default_ns2"], r["fast_ns"] = fr["default_ns"], fr["fast_ns"]
        for m in ("a", "b", "best", "cse"):
            est = r["census_ns"] * (1 - r["saved_share_" + m])
            r["vs_default_" + m] = max(1.0, fr["default_ns"] / est)
            r["vs_fast_" + m] = max(1.0, fr["fast_ns"] / est)
    if not big:
        gen = [t for t in r["terms"] if t["kind"] == "generic"]
        par = max([t["N"] for t in r["terms"] if not t["variate_data"]] or [0])
        if "OP_ISLAND" in r["opaque_ops"] or (r["front_b"][0] >= 40 and not gen
                                                and par < 8):
            b = ("no visible term: UDF / hand-written target terms (opaque "
                 "island or generic ops)")
        elif gen:
            b = "no visible term: one multivariate / categorical-GLM op"
        elif par >= 8:
            b = ("no visible term: variate is active (transformed y, residual "
                 "recursion, missing data)")
        else:
            b = "no visible term: tiny data (N<8)"
        buckets[b].append(r["name"])
    for t in big:
        best = min(t["G_grp"], t["G_b"]) if (t["expfam"] and t["lin"]) else t["G_b"]
        if best <= t["N"] / 2 or t["elem_share"] < 0.1:
            continue
        cls = [c for c in t["classes"] if not c.startswith("y:")]
        if t["kind"] == "generic":
            b = "opaque multivariate/categorical op (only whole-op repeats visible)"
        elif any(c in ("covar", "linpred") for c in cls):
            b = ("per-observation covariate / linear predictor"
                 + (" [groups<=N/2 if the covariate were absent]"
                    if t["G_skel"] <= t["N"] / 2
                    else " [plus per-observation parameters]"))
        elif not t["expfam"] and t["G_grp"] <= t["N"] / 2:
            b = "non-exponential-family density with continuous variate"
        elif t["expfam"] and not t["lin"] and t["G_grp"] <= t["N"] / 2:
            b = "density under nonlinear consumer (mixture/marginalisation/censoring)"
        elif any(c in ("gather", "fn(gather)", "other", "mixed-data/active")
                 for c in cls):
            b = "one latent parameter (or opaque active value) per observation"
        elif all(c == "data" for c in cls):
            b = "all arguments data"
        else:
            b = "other: " + ",".join(cls)
        buckets[b].append(r["name"])
        if (t["op"] == "NORMAL_ID_GLM_LPDF" or (t["op"] == "NORMAL_LPDF"
                and cls and cls[0] == "linpred" and cls[-1] == "bcast")):
            gram.add(r["name"])
    for t in lik:
        if a_ok(t):
            fam_a[t["op"]].add(r["name"])
        if t["G_b"] < t["N"]:
            fam_b[t["op"]].add(r["name"])

P("")
P("== corpus counts: models with >=1 data-variate density term (N>=2) that collapses ==")
for label in ("a_strict", "a_repeated_covariate", "a_any", "b", "b_frontier_only"):
    v = list(H[label].values())
    P(f"{label:22s} any {len(v):3d}; >=2x {sum(x >= 2 for x in v):3d}; "
      f">=10x {sum(x >= 10 for x in v):3d}; >=100x {sum(x >= 100 for x in v):3d}")
sa, sb = set(H["a_any"]), set(H["b"]) | set(H["b_frontier_only"])
P(f"a or b: {len(sa | sb)}; a only: {len(sa - sb)}; b only: {len(sb - sa)}; "
  f"both: {len(sa & sb)}")
P("(a) by opcode (models): " + ", ".join(
    f"{k}:{len(v)}" for k, v in sorted(fam_a.items(), key=lambda kv: -len(kv[1]))))
P("(b) by opcode (models): " + ", ".join(
    f"{k}:{len(v)}" for k, v in sorted(fam_b.items(), key=lambda kv: -len(kv[1]))))
P(f"adjacent shape, not sized: normal likelihood with matvec/GLM linear predictor "
  f"and broadcast sigma that does not collapse 2x (Gram-matrix form): {len(gram)} models")
P("")
P("== estimated whole-gradient ceilings (models at or above each factor) ==")
for key in ("vs_default_a", "vs_default_b", "vs_default_best", "vs_default_cse",
            "vs_fast_a", "vs_fast_b", "vs_fast_best"):
    v = [r[key] for r in ok if key in r]
    P(f"{key:16s} >=1.1x {sum(x >= 1.1 for x in v):3d}; >=1.5x "
      f"{sum(x >= 1.5 for x in v):3d}; >=2x {sum(x >= 2 for x in v):3d}; >=5x "
      f"{sum(x >= 5 for x in v):3d}; >=10x {sum(x >= 10 for x in v):3d}; of {len(v)}")
v = sorted(r["fast_ns"] / r["default_ns2"] for r in ok if "fast_ns" in r)
P(f"runtime fast-math / default ns per gradient: median {v[len(v) // 2]:.3f}; "
  f"<=0.9: {sum(x <= 0.9 for x in v)}; <=0.5: {sum(x <= 0.5 for x in v)}; "
  f">=1.1: {sum(x >= 1.1 for x in v)}")

P("")
P("== near-miss buckets (data-variate terms with N>=8, >=10% of the model's "
  "density elements, <2x collapse); a model can be in several ==")
for b, names in sorted(buckets.items(), key=lambda kv: -len(set(kv[1]))):
    u = sorted(set(names))
    P(f"{len(u):4d}  {b}")
    P("      " + " ".join(u[:14]) + (" ..." if len(u) > 14 else ""))
P(f"source has truncation T[..]: {sum(r['has_trunc'] for r in ok)} "
  f"({' '.join(r['name'] for r in ok if r['has_trunc'])}); graph has cdf-family "
  f"ops: {sum(r['has_cdf'] for r in ok)} "
  f"({' '.join(r['name'] for r in ok if r['has_cdf'])})")
hit = [r for r in ok if r["name"] in (sa | sb)]
P(f"generated quantities mention log_lik: {sum(r['has_loglik_gq'] for r in ok)} "
  f"models; among models with a collapse: "
  f"{sum(r['has_loglik_gq'] for r in hit)} of {len(hit)}")
op = collections.Counter()
for r in ok:
    for k in r["opaque_ops"]:
        op[k] += 1
P("opaque opcodes (models): " + ", ".join(f"{k[3:]}:{v}" for k, v in op.most_common(30)))

P("")
P("== models by estimated ceiling against shipped default ==")
P("columns: est. ceiling vs default for (a) only, (b) only, best of both; best "
  "vs runtime fast-math; cse = ceiling an ideal merge of identical scalar ops "
  "would reach (vs default); default ns/grad; fast/default; census/default")
P(f"{'model':36s} {'a':>6s} {'b':>6s} {'best':>6s} {'vsFast':>6s} {'cse':>6s} "
  f"{'ns/grad':>9s} {'f/d':>5s} {'c/d':>5s} LL  main collapsing terms")
tab = [r for r in ok if "vs_default_best" in r]
tab.sort(key=lambda r: -r["vs_default_best"])
for r in tab:
    if r["vs_default_best"] < 1.1 and r["saved_share_best"] < 0.1:
        continue
    lik = [t for t in r["terms"] if t["variate_data"] and t["N"] >= 2
           and (t["G_b"] < t["N"] or a_ok(t))]
    lik.sort(key=lambda t: -t["N"])
    desc = "; ".join(
        f"{t['op']}({','.join(c for c in t['classes'] if not c.startswith('y:'))})"
        f"{'' if t['n_ops'] < t['N'] else '[scalar ops]'} N={t['N']} Ga="
        f"{t['G_grp'] if a_ok(t) else '-'} Gb={t['G_b']} "
        f"el={t['elem_share']:.2f} t={(t['time_share'] or 0):.2f}"
        for t in lik[:2]) or f"(no density op; target terms {r['front_b'][0]}->{r['front_b'][1]})"
    P(f"{r['name'][:36]:36s} {r['vs_default_a']:6.2f} {r['vs_default_b']:6.2f} "
      f"{r['vs_default_best']:6.2f} {r['vs_fast_best']:6.2f} {r['vs_default_cse']:6.2f} "
      f"{r['default_ns2']:9.0f} {r['fast_ns'] / r['default_ns2']:5.2f} "
      f"{r['census_ns'] / r['default_ns2']:5.2f} {'L' if r['has_loglik_gq'] else '-'}   {desc}")
(HERE / "summary.txt").write_text("\n".join(out) + "\n")
(HERE / "analysis_with_estimates.json").write_text(json.dumps(ok))
print("\n".join(out))
