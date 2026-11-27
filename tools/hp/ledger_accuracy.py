#!/usr/bin/env python3
"""Score the main-corpus entries of tests/ulp_ledger.json against the 80-digit value.

  tools/hp/ledger_accuracy.py HP_DIR [--check BIN] [--only MODEL ...]
      [--switch NAME ...] [--fast-math] [--json OUT] [--write-true-values]

HP_DIR holds hp_eval.py results (one MODEL.json per model, with full
finite-difference gradients, and with --wa for write_array entries). For
each ledger entry that has a reference,
the script runs stanli_check at each recorded point and reports, at the
point and index where stanli is farthest from CmdStan, how many ULPs of the
scale (|lp| for lp, the largest CmdStan gradient entry for a gradient, the
largest CmdStan value of the variable for a write_array column) each
engine is from the true value. It is the measure that the ledger's reason
strings record.

An entry whose value is more than 1e6 scaled ULP from both engines is
UNDECIDED: the reference does not settle which is closer.

--write-true-values fills the ledger's true_values for the main corpus
(TESTING.md, "The rule"). An entry with closer stanli gets them when every
point that CmdStan accepts has an 80-digit value for every value that the
entry covers (a full finite-difference gradient for gradient entries), and
when the distances that its reason states agree with this measure. The
bound is the largest distance of stanli from the true value over the
points. The other entries are listed with the reason that they have none.

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
import re
import subprocess
import sys
import tempfile
from decimal import Decimal
from fractions import Fraction

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import metrics  # noqa: E402
from common import REPO, load_case, v  # noqa: E402


def run(check, stan, dj, pt, env_extra, fast):
    """lp and gradient, followed by {column: value} of the write_array row
    (None if it failed), or None if the run failed."""
    env = dict(os.environ)
    env.update(env_extra)
    cmd = [check, str(stan), str(dj), "--point", str(pt), "--wa-values"]
    if fast:
        cmd.append("--fast-math")
    p = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=1800)
    got = v.parse_status(p.stdout)
    if not got or got[0] != "OK":
        return None
    row = v.parse_wa(p.stdout)
    wa = dict(zip(row[0].split(","), [float(x) for x in row[1]])) if row else None
    return [float(x) for x in got[1:]] + [wa]


def indices(label, n):
    if label == "lp":
        return [0]
    if label.startswith("grad["):
        return [int(label[5:-1])]
    if label == "grad":
        return list(range(1, n))
    return None


def wa_ref(rp):
    """{column: CmdStan value} of a recorded point, or {}."""
    if "wa" not in rp:
        return {}
    return dict(zip(rp["wa"]["names"].split(","), [float(x) for x in rp["wa"]["values"]]))


def wa_columns(label, entries, ref):
    """The write_array columns that the ledger entry `label` covers."""
    if label == "wa":
        return [c for c in ref if "wa " + c not in entries]
    return [label[3:]] if label[3:] in ref else []


def wa_truth(h, column):
    w = h.get("wa") if h else None
    if not w or column not in w["names"]:
        return None
    return metrics.exact(w["values"][w["names"].index(column)])


def pick_wa(label, entries, ref, got):
    """(ulps, point, column, scale) where stanli is farthest from CmdStan."""
    best = None
    for p, rp in ref.items():
        cw, gv = wa_ref(rp), got.get(p)
        gw = gv[-1] if gv else None
        if not cw or not gw:
            continue
        scale = metrics.wa_scales(list(cw), list(cw.values()))
        for c in wa_columns(label, entries, cw):
            sc = scale.get(c.split(".")[0].split(":")[0], 0.0)
            if c in gw:
                u = v.scaled_ulp(cw[c], gw[c], sc)
                if best is None or u > best[0]:
                    best = (u, p, c, sc)
    return best


def score_wa(p, c, sc, rp, gv, h):
    t = wa_truth(h, c)
    gw = gv[-1] if gv else None
    if t is None or not gw or c not in gw:
        return None
    cw = wa_ref(rp)
    return {"point": p, "index": c, "cmdstan": metrics.ulps_from_truth(cw[c], t, sc),
            "stanli": metrics.ulps_from_truth(gw[c], t, sc), "same_as_cmdstan": gw[c] == cw[c]}


def wa_row(model, lab, entries, ref_points, got, configs, pts, all_points):
    """The ledger_accuracy row of a write_array entry, scored like `pick`
    and `score` do a gradient entry."""
    ref = {int(p): rp for p, rp in ref_points.items() if rp.get("values")}
    b = pick_wa(lab, entries, ref, got["default"])
    if b is None:
        return {"model": model, "label": lab, "unscored": "stanli failed or no write_array"}
    _, p, c, sc = b
    hpt = pts.get(str(p))
    if wa_truth(hpt, c) is None:
        var = c.split(".")[0]
        why = ("depends on " + hpt["wa_refused"][var] if var in (hpt or {}).get("wa_refused", {})
               else (hpt or {}).get("wa_error") or "no 80-digit write_array at point %d" % p)
        return {"model": model, "label": lab, "point": p, "index": c, "unscored": why}
    row = {"model": model, "label": lab}
    for name, _ in configs:
        row[name] = score_wa(p, c, sc, ref[p], got[name].get(p), hpt)
    if min(row["default"]["stanli"], row["default"]["cmdstan"]) > 1e6:
        row["undecided"] = True
    if all_points:
        row["all"] = []
        for q, rq in ref.items():
            cw = wa_ref(rq)
            if c in cw:
                scale = metrics.wa_scales(list(cw), list(cw.values()))[c.split(".")[0]]
                r = score_wa(q, c, scale, rq, got["default"].get(q), pts.get(str(q)))
                if r:
                    row["all"].append(r)
    return row


def truth(h, i):
    if i == 0:
        return Fraction(Decimal(h["lp"])) if "lp" in h else None
    if h.get("method") == "fd" and "grad" in h:
        return Fraction(Decimal(h["grad"][i - 1]))
    if str(i) in h.get("grad_coord", {}):
        return Fraction(Decimal(h["grad_coord"][str(i)]))
    return None


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
    return {"point": p, "index": i, "cmdstan": metrics.ulps_from_truth(rv[i], t, sc),
            "stanli": metrics.ulps_from_truth(gv[i], t, sc),
            "same_as_cmdstan": gv[i] == rv[i]}


# The distances that a reason string states, in its three phrasings:
#   "at point 1: stanli 1, CmdStan 15", "at point 2 (grad[7]): stanli 0, ..."
#   "at point 0 b_Intercept is 11.2 (stanli) and 179.2 (CmdStan)"
#   "at point 0 stanli is 8413 and CmdStan 8470"
#   "over the three points stanli is at most 0.9 and CmdStan at most 44.1"
NUM = r"(\d+(?:\.\d+)?)"
STATED = (
    re.compile(r"at point (\d)(?: \(([^)]+)\))?: stanli " + NUM + r", CmdStan " + NUM),
    re.compile(r"at point (\d):? (\S+) is " + NUM + r" \(stanli\) and " + NUM + r" \(CmdStan\)"),
    re.compile(r"at point (\d)()() stanli is " + NUM + r" and CmdStan " + NUM),
)
STATED_ALL = re.compile(r"over the three points stanli is at most " + NUM
                        + r" and CmdStan at most " + NUM
                        + r" scaled ULP from the true value( in the gradient)?")


def stated(label, reason):
    """(point or None for every point, value label, stanli, CmdStan) that a
    reason states, the numbers as written, or None."""
    for pattern in STATED:
        m = pattern.search(reason)
        if m:
            point, name, s, c = m.group(1), m.group(2), m.groups()[-2], m.groups()[-1]
            if name and v.entry_kind(label) == "wa" and not name.startswith("wa "):
                name = "wa " + name
            return int(point), name or label, s, c
    m = STATED_ALL.search(reason)
    if m:
        return None, "grad" if m.group(3) else label, m.group(1), m.group(2)
    return None


def agrees(measured, written):
    """Whether a measured distance rounds to the number that a reason wrote,
    within one unit of its last digit."""
    places = len(written.split(".")[1]) if "." in written else 0
    return abs(measured - float(written)) < 10.0 ** -places


def covered(label, entries, n, columns):
    """The value labels that the ledger entry `label` covers at one point,
    given n lp and gradient values and the write_array columns."""
    if label == "grad":
        return ["grad[%d]" % i for i in range(1, n) if "grad[%d]" % i not in entries]
    if label == "wa":
        return ["wa " + c for c in columns if "wa " + c not in entries]
    if label.startswith("wa "):
        return [label] if label[3:] in columns else []
    return [label]


ZERO_SCALE = "the scale is 0"


def true_value(name, hpt, rv, cw):
    """(decimal string, scale) of one value at one point, or (None, why)."""
    if hpt is None or "lp" not in hpt:
        return None, (hpt or {}).get("skipped") or "no 80-digit value at a point"
    if name == "lp":
        text, scale = hpt["lp"], abs(rv[0])
    elif name.startswith("grad["):
        if hpt.get("method") != "fd" or "grad" not in hpt:
            return None, hpt.get("grad_skipped") or "directional derivatives only"
        text = hpt["grad"][int(name[5:-1]) - 1]
        scale = max([abs(x) for x in rv[1:] if math.isfinite(x)] or [0.0])
    else:
        column = name[3:]
        w = hpt.get("wa")
        if not w or column not in w["names"]:
            var = column.split(".")[0].split(":")[0]
            refused = hpt.get("wa_refused", {})
            return None, ("depends on " + refused[var] if var in refused else
                          hpt.get("wa_error") or "no 80-digit write_array")
        text = w["values"][w["names"].index(column)]
        scale = metrics.wa_scales(list(cw), list(cw.values())).get(
            column.split(".")[0].split(":")[0], 0.0)
    if metrics.exact(text) is None:
        return None, "the true value is not finite"
    if not math.isfinite(scale):
        return None, "the scale is not finite"
    if scale == 0:
        return None, ZERO_SCALE
    return (text, float(scale)), None


def stanli_value(name, gv):
    if gv is None:
        return None
    if name.startswith("wa "):
        return (gv[-1] or {}).get(name[3:])
    return gv[0 if name == "lp" else int(name[5:-1])]


def cmdstan_value(name, rv, cw):
    return cw.get(name[3:]) if name.startswith("wa ") else rv[0 if name == "lp" else int(name[5:-1])]


def truth_entry(model, label, entries, h, ref, got):
    """(true-value entry, None) for one ledger entry, or (None, why).

    A value whose scale is 0 at a point (an output that is exactly 0 in
    CmdStan) gets no true value at that point, and the CmdStan gate keeps
    it there.
    """
    if h is None:
        return None, "no 80-digit run"
    if h.get("error"):
        return None, h["error"][:80]
    why = stated(label, entries[label]["reason"])
    if why is None:
        return None, "the reason states no distances"
    points, dist, apart = {}, {}, {}
    for ps, rp in sorted(ref["points"].items()):
        if not rp.get("values"):
            continue
        p = int(ps)
        rv = [float(x) for x in rp["values"]]
        cw = wa_ref(rp)
        gv = got.get(p)
        names = covered(label, entries, len(rv), list(cw))
        if not names:
            return None, "no value at point %d" % p
        for name in names:
            stored, missing = true_value(name, h.get("points", {}).get(ps), rv, cw)
            if stored is None and missing == ZERO_SCALE:
                continue
            if stored is None:
                return None, missing
            x = stanli_value(name, gv)
            if x is None:
                return None, "stanli gave no value"
            t = metrics.exact(stored[0])
            a = cmdstan_value(name, rv, cw)
            dist[p, name] = (metrics.ulps_from_truth(x, t, stored[1]),
                             metrics.ulps_from_truth(a, t, stored[1]))
            apart[p, name] = v.scaled_ulp(a, x, stored[1])
            points.setdefault(ps, {})[name] = {"value": stored[0], "scale": stored[1]}
    if not points:
        return None, "no value with a positive scale"
    point, name, s, c = why
    if name == "grad" and label != "grad":
        # The reason states the largest distances in the whole gradient.
        for ps, rp in ref["points"].items():
            if rp.get("values"):
                rv = [float(x) for x in rp["values"]]
                for i in range(1, len(rv)):
                    stored, _ = true_value("grad[%d]" % i, h["points"].get(ps), rv, {})
                    x = stanli_value("grad[%d]" % i, got.get(int(ps)))
                    if stored is None or x is None:
                        return None, "the reason states the whole gradient, which has no score"
                    t = metrics.exact(stored[0])
                    dist[int(ps), "grad"] = tuple(
                        max(old, new) for old, new in zip(
                            dist.get((int(ps), "grad"), (0.0, 0.0)),
                            (metrics.ulps_from_truth(x, t, stored[1]),
                             metrics.ulps_from_truth(rv[i], t, stored[1]))))
    if name in v.KIND_ENTRIES and point is not None:
        # The reason states the value where stanli is farthest from CmdStan.
        name = max([k for k in apart if k[0] == point] or [(point, name)],
                   key=lambda k: apart.get(k, 0))[1]
    here = [d for (p, n), d in dist.items() if n == name and point in (None, p)]
    if not here:
        return None, "the reason names %s at point %s, which has no score" % (name, point)
    ds, dc = max(d[0] for d in here), max(d[1] for d in here)
    if min(ds, dc) > 1e6:
        return None, "undecided: both engines are more than 1e6 scaled ULP away"
    if not agrees(dc, c):
        return None, "stale reference: CmdStan is %.1f from the true value, the reason says %s" % (dc, c)
    if not agrees(ds, s):
        return None, "stanli moved: it is %.1f from the true value, the reason says %s" % (ds, s)
    return {"bound": math.ceil(max(d[0] for (p, n), d in dist.items()
                                   if n in points.get(str(p), {}))),
            "method": "fd" if v.entry_kind(label) == "grad" else "direct",
            "platform": v.native_platform(), "points": points}, None


def write_true_values(a, led, refs, tmp):
    """Fill the ledger's true_values for the main corpus from HP_DIR."""
    path = pathlib.Path(a.ledger)
    ledger = v.load_ulp_ledger(path)
    section = ledger["true_values"].setdefault("main", {})
    out, skipped = {}, []
    for model in sorted(led):
        if a.only and model not in a.only:
            continue
        labels = [lab for lab, e in led[model].items() if e["closer"] == "stanli"]
        if not labels:
            continue
        f = pathlib.Path(a.hp) / (model + ".json")
        h = json.loads(f.read_text()) if f.exists() else None
        stan, dj, ref = load_case(model, tmp, refs)
        got = ({int(p): run(a.check, stan, dj, int(p), {}, False)
                for p, rp in ref["points"].items() if rp.get("values")} if h else {})
        for lab in labels:
            entry, why = truth_entry(model, lab, led[model], h, ref, got)
            if entry is None:
                skipped.append((model, lab, why))
                print("%-40s %-18s no true value: %s" % (model, lab, why))
            else:
                out.setdefault(model, {})[lab] = entry
                print("%-40s %-18s bound %d" % (model, lab, entry["bound"]))
        sys.stdout.flush()
    for model in [m for m in section if not a.only or m in a.only]:
        del section[model]
    section.update(out)
    ledger["true_values"]["main"] = dict(sorted(section.items()))
    path.write_text(json.dumps(ledger, indent=1, sort_keys=True) + "\n")
    v.load_ulp_ledger(path)
    print("\n%d entries of %d models have true values; %d stanli entries have none"
          % (sum(map(len, out.values())), len(out), len(skipped)))


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
    ap.add_argument("--write-true-values", action="store_true",
                    help="write the true values of the entries with closer stanli into the "
                         "ledger's true_values, which the replay gates them on")
    a = ap.parse_args()
    led = json.load(open(a.ledger))["platforms"][v.native_platform()]["main"]
    refs, _ = v.replay_refs(v.native_platform(), "clang")
    tmp = tempfile.mkdtemp()
    configs = [("default", {})] + [(s, {"STANLI_" + x.split("=")[0]: (x.split("=") + ["1"])[1]
                                        for x in s.split("+")}) for s in a.switch]
    if a.census:
        return census(a, configs, refs, tmp)
    if a.write_true_values:
        return write_true_values(a, led, refs, tmp)
    out = []
    for model in sorted(led):
        if a.only and model not in a.only:
            continue
        f = pathlib.Path(a.hp) / (model + ".json")
        h = json.loads(f.read_text()) if f.exists() else None
        pts = (h or {}).get("points", {})
        usable = {p for p, o in pts.items() if "lp" in o}
        labels = list(led[model])
        has_wa = any("wa" in o for o in pts.values())
        lab_ok = [lab for lab in labels if indices(lab, 2) is not None or has_wa]
        if not usable or not lab_ok:
            for lab in labels:
                why = ("no 80-digit run" if h is None else h.get("error") or
                       ("no 80-digit write_array (hp_eval.py --wa)" if indices(lab, 2) is None
                        else "no points"))
                out.append({"model": model, "label": lab, "unscored": why})
        else:
            stan, dj, ref = load_case(model, tmp, refs)
            rv = {int(p): [float(x) for x in ref["points"][p]["values"]]
                  for p in ref["points"] if ref["points"][p].get("values")}
            got = {name: {p: run(a.check, stan, dj, p, env, a.fast_math) for p in rv}
                   for name, env in configs}
            for lab in labels:
                if indices(lab, 2) is None:
                    out.append(wa_row(model, lab, led[model], ref["points"], got, configs, pts, a.all_points))
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
            print("%-40s %-10s p%s i%-3s stanli %8.1f cmdstan %8.1f %-9s %s" % (
                r["model"], r["label"], d["point"], d["index"], d["stanli"], d["cmdstan"], flag, cols))
            for x in r.get("all", []):
                print("    p%d i%-3s stanli %8.1f cmdstan %8.1f" % (x["point"], x["index"], x["stanli"], x["cmdstan"]))
        sys.stdout.flush()
    if a.json:
        pathlib.Path(a.json).write_text(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
