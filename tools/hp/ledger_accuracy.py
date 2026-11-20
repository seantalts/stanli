#!/usr/bin/env python3
"""Score the main-corpus entries of tests/ulp_ledger.json against the 80-digit value.

  tools/hp/ledger_accuracy.py HP_DIR [--check BIN] [--only MODEL ...]
      [--switch NAME ...] [--fast-math] [--json OUT]

HP_DIR holds hp_eval.py results (one MODEL.json per model, with full
finite-difference gradients). For each ledger entry that has a reference,
the script runs stanli_check at each recorded point and reports, at the
point and index where stanli is farthest from CmdStan, how many ULPs of the
scale (|lp| for lp, the largest CmdStan gradient entry for a gradient) each
engine is from the true value. It is the measure that the ledger's reason
strings record.

An entry whose value is more than 1e6 scaled ULP from both engines is
UNDECIDED: the reference does not settle which is closer.

--switch NAME runs stanli_check once more per name with STANLI_NAME=1 set
(for example NO_REROLL, REGION_MAP=0 for a switch that needs a value, or
NO_REROLL+NO_PARTITION for several at once),
and adds a column per switch. --only limits the
run to the named models. Entries without a reference are listed with the
reason that the evaluator gave.
"""
import argparse
import json
import math
import os
import pathlib
import subprocess
import sys
import tempfile
from decimal import Decimal
from fractions import Fraction

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from common import REPO, load_case, v  # noqa: E402


def run(check, stan, dj, pt, env_extra, fast):
    env = dict(os.environ)
    env.update(env_extra)
    cmd = [check, str(stan), str(dj), "--point", str(pt)]
    if fast:
        cmd.append("--fast-math")
    p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=1800)
    got = v.parse_status(p.stdout)
    if not got or got[0] != "OK":
        return None
    return [float(x) for x in got[1:]]


def indices(label, n):
    if label == "lp":
        return [0]
    if label.startswith("grad["):
        return [int(label[5:-1])]
    if label == "grad":
        return list(range(1, n))
    return None


def truth(h, i):
    if i == 0:
        return Fraction(Decimal(h["lp"])) if "lp" in h else None
    if h.get("method") == "fd" and "grad" in h:
        return Fraction(Decimal(h["grad"][i - 1]))
    if str(i) in h.get("grad_coord", {}):
        return Fraction(Decimal(h["grad_coord"][str(i)]))
    return None


def err(x, t, scale):
    return float(abs(Fraction(x) - t) / Fraction(math.ulp(scale)))


def pick(label, ref, got):
    """(point, index) where the default run is farthest from CmdStan, as the ledger's bound."""
    best = None
    for p, rv in ref.items():
        gv = got.get(p)
        if gv is None:
            continue
        S = max([abs(x) for x in rv[1:] if math.isfinite(x)] or [0])
        for i in indices(label, len(rv)) or []:
            sc = abs(rv[0]) if i == 0 else S
            u = v.scaled_ulp(rv[i], gv[i], sc)
            if best is None or u > best[0]:
                best = (u, p, i, sc)
    return best


def score(p, i, sc, rv, gv, h):
    """Distances of CmdStan and stanli from the truth at (p, i), in ULPs of the scale."""
    t = truth(h, i) if h else None
    if t is None or gv is None or not sc:
        return None
    return {"point": p, "index": i, "cmdstan": err(rv[i], t, sc), "stanli": err(gv[i], t, sc)}


def census(a, configs, refs, tmp):
    """Score every value of the --only models at every point, not only the ledger's entries."""
    tot = {}
    for model in a.only:
        h = json.loads((pathlib.Path(a.hp) / (model + ".json")).read_text())
        stan, dj, ref = load_case(model, tmp, refs)
        for ps, rp in ref["points"].items():
            hpt = h.get("points", {}).get(ps)
            if not hpt or "lp" not in hpt or not rp.get("values"):
                continue
            p = int(ps)
            rv = [float(x) for x in rp["values"]]
            S = max([abs(x) for x in rv[1:] if math.isfinite(x)] or [0])
            for name, env in configs:
                gv = run(a.check, stan, dj, p, env, a.fast_math)
                for i in range(len(rv)):
                    r = score(p, i, abs(rv[0]) if i == 0 else S, rv, gv, hpt)
                    if r is None:
                        continue
                    t = tot.setdefault((model, name), [0, 0, 0, 0.0, 0.0])
                    t[0 if r["stanli"] < r["cmdstan"] else 1 if r["stanli"] > r["cmdstan"] else 2] += 1
                    t[3] += r["stanli"]
                    t[4] += r["cmdstan"]
    print("%-28s %-16s %6s %6s %6s %10s %10s" % ("model", "config", "closer", "farther", "tie",
                                                 "sum_stanli", "sum_cmdstan"))
    for (model, name), t in tot.items():
        print("%-28s %-16s %6d %6d %6d %10.1f %10.1f" % ((model, name) + tuple(t)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("hp")
    ap.add_argument("--check", default=str(REPO / "build-rel" / "stanli_check"))
    ap.add_argument("--ledger", default=str(REPO / "tests" / "ulp_ledger.json"))
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--switch", nargs="*", default=[])
    ap.add_argument("--fast-math", action="store_true")
    ap.add_argument("--all-points", action="store_true",
                    help="also print both distances at every point for the entry's indices")
    ap.add_argument("--census", action="store_true",
                    help="score every value of the --only models at every point and count, per "
                         "configuration, how often stanli is closer to the truth than CmdStan")
    ap.add_argument("--json")
    a = ap.parse_args()
    led = json.load(open(a.ledger))["platforms"][v.native_platform()]["main"]
    refs, _ = v.replay_refs(v.native_platform(), "clang")
    tmp = tempfile.mkdtemp()
    configs = [("default", {})] + [(s, {"STANLI_" + x.split("=")[0]: (x.split("=") + ["1"])[1]
                                        for x in s.split("+")}) for s in a.switch]
    if a.census:
        return census(a, configs, refs, tmp)
    out = []
    for model in sorted(led):
        if a.only and model not in a.only:
            continue
        f = pathlib.Path(a.hp) / (model + ".json")
        h = json.loads(f.read_text()) if f.exists() else None
        pts = (h or {}).get("points", {})
        usable = {p for p, o in pts.items() if "lp" in o}
        labels = list(led[model])
        lab_ok = [lab for lab in labels if indices(lab, 2) is not None]
        if not usable or not lab_ok:
            for lab in labels:
                why = ("no 80-digit run" if h is None else h.get("error") or
                       ("write_array value" if indices(lab, 2) is None else "no points"))
                out.append({"model": model, "label": lab, "unscored": why})
        else:
            stan, dj, ref = load_case(model, tmp, refs)
            rv = {int(p): [float(x) for x in ref["points"][p]["values"]]
                  for p in ref["points"] if ref["points"][p].get("values")}
            got = {name: {p: run(a.check, stan, dj, p, env, a.fast_math) for p in rv}
                   for name, env in configs}
            for lab in labels:
                if indices(lab, 2) is None:
                    out.append({"model": model, "label": lab, "unscored": "write_array value"})
                    continue
                b = pick(lab, rv, got["default"])
                if b is None:
                    out.append({"model": model, "label": lab, "unscored": "stanli failed"})
                    continue
                _, p, i, sc = b
                hpt = pts.get(str(p))
                if hpt is None or truth(hpt, i) is None:
                    why = (h.get("error") if h.get("error") else
                           "no 80-digit value at point %d" % p if hpt is None or "lp" not in hpt
                           else hpt.get("grad_skipped") or "directional derivatives only")
                    out.append({"model": model, "label": lab, "point": p, "index": i, "unscored": why})
                    continue
                row = {"model": model, "label": lab}
                for name, _ in configs:
                    row[name] = score(p, i, sc, rv[p], got[name].get(p), hpt)
                if min(row["default"]["stanli"], row["default"]["cmdstan"]) > 1e6:
                    row["undecided"] = True
                if a.all_points:
                    row["all"] = []
                    for q, rq in rv.items():
                        hq = pts.get(str(q))
                        Sq = max([abs(x) for x in rq[1:] if math.isfinite(x)] or [0])
                        for j in indices(lab, len(rq)):
                            r = score(q, j, abs(rq[0]) if j == 0 else Sq, rq, got["default"].get(q), hq)
                            if r:
                                row["all"].append(r)
                out.append(row)
        for r in out[-len(labels):]:
            if "unscored" in r:
                print("%-40s %-10s unscored: %s" % (r["model"], r["label"], r["unscored"]))
                continue
            d = r["default"]
            if d is None:
                print("%-40s %-10s no score" % (r["model"], r["label"]))
                continue
            cols = " ".join("%s=%.0f" % (n, r[n]["stanli"]) for n, _ in configs[1:] if r.get(n))
            flag = ("UNDECIDED" if r.get("undecided") else
                    "FARTHER" if d["stanli"] > d["cmdstan"] else "ok")
            print("%-40s %-10s p%s i%-3d stanli %8.1f cmdstan %8.1f %-9s %s" % (
                r["model"], r["label"], d["point"], d["index"], d["stanli"], d["cmdstan"], flag, cols))
            for x in r.get("all", []):
                print("    p%d i%-3d stanli %8.1f cmdstan %8.1f" % (x["point"], x["index"], x["stanli"], x["cmdstan"]))
        sys.stdout.flush()
    if a.json:
        pathlib.Path(a.json).write_text(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
