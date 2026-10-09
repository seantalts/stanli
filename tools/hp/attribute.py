#!/usr/bin/env python3
"""Compare stanli --fast-math with default mode, and find which fast-mode pass moves the result.

  tools/hp/attribute.py RESULTS_DIR MODEL [MODEL ...] [--out DIR]

Uses the high-precision values stored by hp_eval.py. For each model it reports
whether fast mode returns bit-identical values to default at every scored
point, the distance between the two (gate metric), and, for models where they
differ, the error against the reference with each STANLI_NO_* switch set in
turn. A switch "restores" default when fast mode with it set returns exactly
the default values.
"""
import argparse
import json
import os
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import hp_eval  # noqa: E402
import metrics  # noqa: E402
from common import REPO, load_case, v  # noqa: E402

TOGGLES = {t: {"STANLI_NO_" + t: "1"} for t in
           ("COLLAPSE", "CSE", "PARTITION", "REROLL", "FUSED_DENSITY", "GP_DIAGONAL_FUSION", "ISLAND")}
CHECK = [str(REPO / "build-rel" / "stanli_check")]


def run(stan, dj, pt, fast, toggle):
    env = dict(os.environ)
    if toggle:
        env.update(TOGGLES[toggle])
    cmd = CHECK + [str(stan), str(dj), "--point", str(pt)]
    if fast:
        cmd.append("--fast-math")
    p = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO, env=env, timeout=900)
    got = v.parse_status(p.stdout)
    if not got or got[0] != "OK" or not v.accepted(got):
        return None
    return [float(x) for x in got[1:]]


def reference(res, o):
    if o["method"] == "fd":
        return [float(x) for x in o["grad"]], None
    return None, [{"v": metrics.direction(x["seed"], res["n"]), "D": float(x["D_hp"])} for x in o["dirs"]]


def distance(a, b):
    if a is None or b is None:
        return 0.0 if a is b else float("inf")
    return max(metrics.lp_error(a[0], b[0]), metrics.grad_error(a[1:], b[1:]))


def worst(res, vals_by_pt):
    lp, gr = [], []
    for pt, vals in vals_by_pt.items():
        o = res["points"][pt]
        hg, dirs = reference(res, o)
        e = hp_eval.arm_errors(vals, float(o["lp"]), hg, dirs, o["grad_scale"])
        if "lp" in e:
            lp.append(e["lp"])
            gr.append(e["grad"])
    return (max(lp) if lp else None), (max(gr) if gr else None)


def analyse(m, res, refs, tmp):
    stan, dj, _ = load_case(m, tmp, refs)
    pts = [pt for pt, o in res["points"].items() if "arms" in o and "grad" in o["arms"].get("cmdstan", {})]
    default = {pt: run(stan, dj, int(pt), False, None) for pt in pts}
    fast = {pt: run(stan, dj, int(pt), True, None) for pt in pts}
    out = {"model": m, "n": res["n"], "points": pts}
    out["identical"] = all(default[pt] == fast[pt] for pt in pts)
    out["distance"] = max([distance(default[pt], fast[pt]) for pt in pts] or [0.0])
    out["default"] = worst(res, default)
    out["fast"] = worst(res, fast)
    if not out["identical"]:
        out["toggles"] = {}
        for t in TOGGLES:
            vals = {pt: run(stan, dj, int(pt), True, t) for pt in pts}
            lp, gr = worst(res, vals)
            out["toggles"][t] = {"restores": all(vals[pt] == default[pt] for pt in pts),
                                 "changes": any(vals[pt] != fast[pt] for pt in pts), "lp": lp, "grad": gr}
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("results")
    ap.add_argument("models", nargs="+")
    ap.add_argument("--out")
    ap.add_argument("--check", help="stanli_check binary (default build-rel/stanli_check)")
    ap.add_argument("--extra-toggles", action="store_true",
                    help="also try the partial-evaluation split; needs a stanli_check that reads STANLI_ATTR_PE")
    a = ap.parse_args()
    if a.check:
        CHECK[0] = a.check
    if a.extra_toggles:
        TOGGLES["PE_OFF"] = {"STANLI_ATTR_PE": "off"}
        TOGGLES["PE_ONLY"] = {"STANLI_ATTR_PE": "only"}
    refs, _ = v.replay_refs(v.native_platform(), "clang")
    tmp = tempfile.mkdtemp()
    for m in a.models:
        res = json.loads((pathlib.Path(a.results) / (m + ".json")).read_text())
        if "points" not in res or not any("arms" in o for o in res["points"].values()):
            continue
        try:
            r = analyse(m, res, refs, tmp)
        except Exception as e:
            r = {"model": m, "error": "%s: %s" % (type(e).__name__, str(e)[:200])}
        if a.out:
            pathlib.Path(a.out).mkdir(parents=True, exist_ok=True)
            (pathlib.Path(a.out) / (m + ".json")).write_text(json.dumps(r))
        if "error" in r:
            print(m, r["error"])
        elif r["identical"]:
            print("%-28s identical" % m)
        else:
            rs = [t for t, x in r["toggles"].items() if x["restores"]]
            print("%-28s distance %.2e  default %s  fast %s  restored by %s" % (
                m, r["distance"], r["default"], r["fast"], ",".join(rs) or "-"))
        sys.stdout.flush()


if __name__ == "__main__":
    main()
