#!/usr/bin/env python3
"""Summarize a directory of hp_eval.py JSON files as markdown tables.

  tools/hp/report.py DIR [--top 25]
"""
import argparse
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
import metrics  # noqa: E402
import verify_refs  # noqa: E402

ARMS = ("cmdstan", "default", "fast")

NORMAL_LCDF = ("Stan Math's normal_lcdf, normal_lccdf and std_normal_lcdf gradients are approximate "
               "(relative error 2e-7 to 7e-7 at sample arguments); the three arms agree")
DIAGNOSIS = {
    "cm_lba1": NORMAL_LCDF,
    "s2_cens_interval": NORMAL_LCDF,
    "s2_cumulative_probit": NORMAL_LCDF,
    "s2_mi_trunc_lb": NORMAL_LCDF,
    "s2_weights_trunc": NORMAL_LCDF,
    "sw_cens": NORMAL_LCDF,
    "sw_trunc": NORMAL_LCDF,
    "hmm_drive_0": "forward recursion over the data; double-precision gradient error 1e-11 of the largest entry, equal in all arms",
    "hmm_drive_1": "forward recursion over the data; double-precision gradient error 1e-11 of the largest entry, equal in all arms",
    "hmm_gaussian": "forward recursion over the data; double-precision gradient error 1e-11 of the largest entry, equal in all arms",
    "iohmm_reg": "forward recursion over the data; double-precision gradient error 1e-11 of the largest entry, equal in all arms",
    "logistic_regression_rhs": "bernoulli_logit_glm drops log1p(exp(eta)) for eta below -20, up to 2e-9 per observation; mirroring the cutoff brings the log density to 1e-14",
    "s2_com_poisson": "gradient of shape at points 1 and 2 is 0 in all arms because the model branches on nu == 1 (excluded)",
    "kronecker_gp": "eigendecomposition of repeated eigenvalues; 435 of 438 gradients non-finite in CmdStan at point 2 (excluded there)",
    "i320_gp_expquad": "Cholesky pivot 3.7e-12; reference converged (80 against 160 digits)",
    "s2_gp_by_gr": "Cholesky pivot 1.8e-12; reference converged",
    "sw_gp": "Cholesky pivot 1.1e-12; reference converged",
}

EXCLUDED = {
    ("s2_com_poisson", "1", "grad"): "nu == 1 branch",
    ("s2_com_poisson", "2", "grad"): "nu == 1 branch",
    ("kronecker_gp", "2", "grad"): "non-finite reference gradient",
}


def load(d):
    out = {}
    for p in sorted(pathlib.Path(d).glob("*.json")):
        r = json.loads(p.read_text())
        out[r["model"]] = r
    return out


def arm_max(res, arm, key):
    xs, rej = [], 0
    for pt, o in res.get("points", {}).items():
        if (res["model"], pt, key) in EXCLUDED:
            continue
        a = o.get("arms", {}).get(arm)
        if a is None:
            continue
        if a.get("rejected"):
            rej += 1
        elif key in a:
            xs.append(a[key])
    return (max(xs) if xs else None), rej


def scored(res):
    return [o for o in res.get("points", {}).values() if "arms" in o]


def summarize(results):
    rows = {}
    for m, r in results.items():
        row = {"model": m, "n": r.get("n"), "error": r.get("error")}
        sp = scored(r)
        row["points"] = len(sp)
        row["method"] = sorted({o.get("method") for o in sp if o.get("method")})
        for a in ARMS:
            for k in ("lp", "grad"):
                row[a, k], row[a, "rej"] = arm_max(r, a, k)
        row["rej_parity"] = sum(
            1 for o in sp if o["arms"].get("cmdstan", {}).get("rejected") != o["arms"].get("default", {}).get("rejected")
            or o["arms"].get("default", {}).get("rejected") != o["arms"].get("fast", {}).get("rejected"))
        diffs = [o["lp_dps_diff"] for o in sp if "lp_dps_diff" in o]
        row["dps_diff"] = max(diffs) if diffs else None
        errs = lambda a: [x for x in (row[a, "lp"], row[a, "grad"]) if x is not None]
        row["validity"] = metrics.validity(errs("cmdstan"), errs("default"), m in verify_refs.ILL_CONDITIONED) if sp else "unscored"
        rows[m] = row
    return rows


def fmt(x):
    return "-" if x is None else "%.2e" % x


def dist(xs):
    xs = [x for x in xs if x is not None]
    return [metrics.percentile(xs, q) for q in (50, 90, 99, 100)]


def table(head, body):
    print("| " + " | ".join(head) + " |")
    print("|" + "|".join("---" for _ in head) + "|")
    for b in body:
        print("| " + " | ".join(str(c) for c in b) + " |")
    print()


def drivers(a):
    t = a["toggles"]
    got = [k for k in ("COLLAPSE", "CSE", "PARTITION", "REROLL", "FUSED_DENSITY", "GP_DIAGONAL_FUSION", "ISLAND", "PE_OFF") if t.get(k, {}).get("restores")]
    names = {"PE_OFF": "partial evaluation"}
    if got:
        return ", ".join(names.get(g, g.lower()) for g in got)
    moved = [k for k in ("COLLAPSE", "CSE", "PARTITION", "REROLL", "FUSED_DENSITY", "GP_DIAGONAL_FUSION", "ISLAND") if t.get(k, {}).get("changes")]
    if len(moved) == 1:
        return moved[0].lower() + " (low bits differ from default)"
    return "several: " + ", ".join(m.lower() for m in moved) if moved else "unresolved"


def attribution(d, valid, top):
    rows = {}
    for p in sorted(pathlib.Path(d).glob("*.json")):
        r = json.loads(p.read_text())
        if "error" not in r:
            rows[r["model"]] = r
    print("## Fast against default, all scored models\n")
    same = [r for r in rows.values() if r["identical"]]
    diff = [r for r in rows.values() if not r["identical"]]
    dist = sorted(r["distance"] for r in diff)
    print("%d scored models; fast returns bit-identical values to default on %d and differs on %d. "
          "Gate-metric distance between the two over the differing models: median %s, p90 %s, max %s.\n" % (
              len(rows), len(same), len(diff), fmt(metrics.percentile(dist, 50)), fmt(metrics.percentile(dist, 90)),
              fmt(metrics.percentile(dist, 100))))
    tally = {}
    for r in diff:
        if "PE_OFF" not in r["toggles"]:
            continue
        key = drivers(r)
        tally[key] = tally.get(key, 0) + 1
    print("Fast-mode feature whose removal gives back default's values exactly:\n")
    table(["feature", "models"], sorted(tally.items(), key=lambda kv: -kv[1]))
    print("### Largest fast-to-default distances\n")
    table(["model", "n", "distance", "default error (lp, grad)", "fast error (lp, grad)", "driver"],
          [(r["model"], r["n"], fmt(r["distance"]), "%s, %s" % tuple(fmt(x) for x in r["default"]),
            "%s, %s" % tuple(fmt(x) for x in r["fast"]), drivers(r) if "PE_OFF" in r["toggles"] else "")
           for r in sorted(diff, key=lambda r: -r["distance"])[:top]])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--attr", help="attribute.py output directory")
    args = ap.parse_args()
    results = load(args.dir)
    rows = summarize(results)
    all_models = [l.strip() for l in open(pathlib.Path(args.dir).parent / "models.txt")] if (pathlib.Path(args.dir).parent / "models.txt").exists() else list(rows)

    print("## Coverage\n")
    unc = [(m, results[m].get("error", "no scored point") if m in results else "not run") for m in all_models
           if m not in rows or rows[m]["validity"] == "unscored"]
    cov = [m for m in rows if m not in dict(unc)]
    print("%d models in the corpus, %d scored, %d not.\n" % (len(all_models), len(cov), len(all_models) - len(cov)))
    table(["model", "reason"], [(m, str(e)[:110]) for m, e in unc])
    meth = {}
    for m in cov:
        for k in rows[m]["method"]:
            meth[k] = meth.get(k, 0) + 1
    print("Gradient reference: %s.\n" % ", ".join("%s %d models" % kv for kv in sorted(meth.items())))

    print("## Validity gate\n")
    counts = {}
    for m in cov:
        counts[rows[m]["validity"]] = counts.get(rows[m]["validity"], 0) + 1
    print(", ".join("%s %d" % kv for kv in sorted(counts.items())) + ".\n")
    bad = [rows[m] for m in cov if rows[m]["validity"] != "valid"]
    table(["model", "n", "status", "cmdstan lp", "cmdstan grad", "default lp", "default grad", "fast lp", "fast grad", "lp dps diff", "cause"],
          [(b["model"], b["n"], b["validity"], fmt(b["cmdstan", "lp"]), fmt(b["cmdstan", "grad"]),
            fmt(b["default", "lp"]), fmt(b["default", "grad"]), fmt(b["fast", "lp"]), fmt(b["fast", "grad"]),
            fmt(b["dps_diff"]), DIAGNOSIS.get(b["model"], "")) for b in sorted(bad, key=lambda b: b["model"])])

    valid = [rows[m] for m in cov if rows[m]["validity"] == "valid"]
    print("## Error distribution over %d valid models (max over points)\n" % len(valid))
    body = []
    for a in ARMS:
        for k in ("lp", "grad"):
            body.append([a, k] + [fmt(x) for x in dist([r[a, k] for r in valid])])
    table(["arm", "quantity", "median", "p90", "p99", "max"], body)

    print("## Fast against default\n")
    tally = {}
    cmp = {}
    for r in valid:
        for k in ("lp", "grad"):
            d, f = r["default", k], r["fast", k]
            if d is None or f is None:
                continue
            c = metrics.compare(d, f)
            cmp[r["model"], k] = c
            tally[k, c] = tally.get((k, c), 0) + 1
    table(["quantity", "fast worse", "fast better", "similar"],
          [[k] + [tally.get((k, c), 0) for c in ("fast_worse", "fast_better", "similar")] for k in ("lp", "grad")])
    print("Rule: fast_worse when fast error > %gx default error and > %g; fast_better is the mirror.\n" % (metrics.RATIO, metrics.FLOOR))

    for c, title in (("fast_worse", "Fast less accurate than default"), ("fast_better", "Fast more accurate than default")):
        sel = [r for r in valid if any(cmp.get((r["model"], k)) == c for k in ("lp", "grad"))]
        sel.sort(key=lambda r: -max(abs((r["fast", k] or 0) - (r["default", k] or 0)) for k in ("lp", "grad")))
        print("### %s (%d models)\n" % (title, len(sel)))
        table(["model", "n", "default lp", "fast lp", "default grad", "fast grad"],
              [(r["model"], r["n"], fmt(r["default", "lp"]), fmt(r["fast", "lp"]), fmt(r["default", "grad"]), fmt(r["fast", "grad"]))
               for r in sel[:args.top]])

    print("### Fast above default by more than 1e-15 (top %d by increase)\n" % args.top)
    inc = []
    for r in valid:
        for k in ("lp", "grad"):
            d, f = r["default", k], r["fast", k]
            if d is not None and f is not None and f - d > 1e-15:
                inc.append((f - d, r, k))
    inc.sort(key=lambda x: -x[0])
    table(["model", "n", "quantity", "default", "fast"],
          [(r["model"], r["n"], k, fmt(r["default", k]), fmt(r["fast", k])) for _, r, k in inc[:args.top]])

    if args.attr:
        attribution(args.attr, valid, args.top)

    par = [r for r in rows.values() if r["rej_parity"]]
    print("## Rejection parity breaks: %d models\n" % len(par))
    table(["model", "points differing"], [(r["model"], r["rej_parity"]) for r in par])


if __name__ == "__main__":
    main()
