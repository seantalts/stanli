#!/usr/bin/env python3
"""How far is `stanli_check --interp` from the compiled path?

For each model this emits the shipping MIR, then runs stanli_check at the
same points twice: once through the compiled path and once with --interp,
which runs the whole model through the MIR interpreter on doubles with a
finite-difference gradient. It compares lp, the gradient (within the error
the interpreter estimates for it), the write_array row, the printed output
and the error messages, and writes one JSON file per corpus plus a table.

With --interp-o0 the interpreter reads the `--passes none` MIR instead.
Where that disagrees with the compiled path, the interpreter runs again on
the shipping MIR: agreement there puts the difference in a stanc3 pass, and
disagreement in the runtime.

    tools/interp_gap.py --out DIR [--corpus NAME ...] [--jobs N] [--interp-o0]

NAME is fixtures, lit, stanc3_good, posteriordb, brms, rethinking, cogmod,
other_local (the remaining tests/ collections), or dir:PATH for every
PATH/*.stan with its PATH/*.json.
"""
from __future__ import annotations

import argparse
import collections
import concurrent.futures
import gzip
import json
import math
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import threading
import time

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
from corpus_inventory import corpus_cases, materialize_data  # noqa: E402
import check_stanc3_good as good  # noqa: E402
import check_status  # noqa: E402
from check_compare import ABS_TOL, printed_lines  # noqa: E402

POINTS = "0,1,2,3,4"
# The gradient is compared at the two pseudorandom points only: the others
# sit on round values (2 is all zeros) where Stan Math's special cases make
# its derivative differ from the derivative of its value (pow at 0, an LKJ
# eta of 1). A gradient mismatch counts when it shows at both.
GRAD_POINTS = ("3", "4")
GRAD_MAX = 200
TOLS = (1e-10, 1e-8)
# A gradient component agrees when |compiled - interp| is within
# GRAD_ERR_FACTOR times the interpreter's error estimate plus
# GRAD_REL_TOL * max(1, |compiled|).
GRAD_ERR_FACTOR = 10.0
GRAD_REL_TOL = 1e-6


def worst_rel(xs, ys):
    w = 0.0
    for a, b in zip(xs, ys):
        a, b = float(a), float(b)
        if math.isnan(a) and math.isnan(b) or (a == b):
            continue
        if not (math.isfinite(a) and math.isfinite(b)):
            return math.inf
        d = abs(a - b)
        if d <= ABS_TOL:
            continue
        w = max(w, d / max(abs(a), abs(b)))
    return w


def cases(names, pdb, tmp):
    out = []
    want = set(names)
    if "fixtures" in want:
        for stan in sorted((REPO / "tests" / "fixtures").glob("*.stan")):
            data = stan.with_suffix(".json")
            if not data.exists():
                data = tmp / "empty.json"
            out.append(("fixtures", stan.stem, stan, data))
    if "lit" in want:
        for stan in sorted((REPO / "tests" / "lit").rglob("*.stan")):
            text = stan.read_text()
            m = re.search(r"^// STANLI-LIT-DATA: (.*)$", text, re.M)
            data = tmp / f"lit_{stan.parent.name}_{stan.stem}.json"
            data.write_text(m.group(1) if m else "{}")
            out.append(("lit", f"{stan.parent.name}/{stan.stem}", stan, data))
    if "stanc3_good" in want:
        gdata = json.loads(gzip.decompress(good.DATA.read_bytes()))
        overrides = json.loads(good.OVERRIDES.read_text())
        for path in good.corpus_models():
            source, obj = good.model_input(path, overrides, gdata)
            if obj is None:
                continue
            data = tmp / ("good_" + path.replace("/", "__") + ".json")
            data.write_text(json.dumps(obj))
            out.append(("stanc3_good", path, source, data))
    for name in sorted(n for n in want if n.startswith("dir:")):
        root = pathlib.Path(name[4:])
        for stan in sorted(root.glob("*.stan")):
            data = stan.with_suffix(".json")
            out.append((root.name, stan.stem, stan,
                        data if data.exists() else tmp / "empty.json"))
    collections_wanted = {n for n in want if not n.startswith("dir:")} - {
        "fixtures", "lit", "stanc3_good"}
    if collections_wanted:
        for name, case in corpus_cases(pdb, "all", include_language=True).items():
            coll = case.collection
            group = coll if coll in ("posteriordb", "brms", "rethinking",
                                     "cogmod") else "other_local"
            if group in collections_wanted:
                out.append((group, name, case.source,
                            materialize_data(case, tmp)))
    return out


MAX_RSS_KB = 6 * 1024 * 1024


def run(argv, timeout, env=None):
    """(code, stdout, stderr, wall); code None on timeout, "rss" when the
    process passed MAX_RSS_KB and was killed."""
    t = time.perf_counter()
    p = subprocess.Popen([str(a) for a in argv], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, errors="replace",
                         env=env)
    killed = []

    def watch():
        while p.poll() is None:
            try:
                rss = int(subprocess.run(["ps", "-o", "rss=", "-p", str(p.pid)],
                                         capture_output=True, text=True).stdout or 0)
            except ValueError:
                rss = 0
            if rss > MAX_RSS_KB:
                killed.append(rss)
                p.kill()
                return
            time.sleep(0.5)
    w = threading.Thread(target=watch, daemon=True)
    w.start()
    try:
        out, err = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        p.communicate()
        return None, "", "", time.perf_counter() - t
    if killed:
        return "rss", out, err, time.perf_counter() - t
    return p.returncode, out, err, time.perf_counter() - t


def grad_errors(out):
    """{point: [error, ...]} from the GRAD_ERR lines of --interp."""
    errs, current = {}, "pre"
    for line in out.splitlines():
        head, _, tail = line.partition(" ")
        if head == "POINT" and tail.strip().isdigit():
            current = str(int(tail))
        elif head == "GRAD_ERR":
            errs[current] = tail.split()
    return errs


def summarize(proc, status_path=None):
    code, out, err, wall = proc
    records = check_status.read(status_path) if status_path else \
        check_status.CheckStatus()
    if code is None:
        return {"state": "timeout", "wall": wall}
    if code == "rss":
        return {"state": "memory", "wall": wall,
                "message": f"killed above {MAX_RSS_KB // 1024 // 1024} GB RSS"}
    parsed = good.parse_output(out)
    pre = parsed["pre"]
    res = {"wall": wall, "code": code}
    if pre and pre[0] == "COMPILE_FAIL":
        res.update(state="compile_fail", message=pre[1][:300],
                   error=(records.compile or {}).get("error"))
        return res
    if not parsed["points"] or code not in (0, 1):
        tail = (err or out).strip().splitlines()[-1:] or [f"exit {code}"]
        res.update(state="crash", message=tail[0][:300])
        return res
    pts = {}
    errs = grad_errors(out)
    prints = printed_lines(out)
    res["prints"] = {str(k): [x for x in v if not x.startswith("GRAD_ERR")]
                     for k, v in prints.items()}
    for k, sec in parsed["points"].items():
        st = sec["status"]
        p = {}
        if st is None:
            p["status"] = "none"
        elif st[0] == "OK":
            p["status"] = "ok"
            p["lp"] = st[1][0]
            p["grad"] = st[1][1:]
            if str(k) in errs:
                p["grad_err"] = errs[str(k)]
        else:
            p["status"] = "threw"
            p["message"] = st[1][:300]
            p["error"] = records.points.get(k, {}).get("error")
        wa = sec["wa"]
        if wa and wa[0] == "ok":
            p["wa"] = {"names": wa[1], "values": wa[2]}
        elif wa:
            p["wa"] = {"threw": wa[1][:300], "error": (
                records.points.get(k, {}).get("write_array") or {}).get(
                    "error")}
        pts[str(k)] = p
    res.update(state="points", points=pts)
    ms = re.findall(r"INTERP_TIME point \d+ lp_ms ([\d.]+) grad_ms ([\d.]+) "
                    r"wa_ms ([\d.]+)", err)
    if ms:
        res["lp_ms"] = [float(a) for a, _, _ in ms]
        res["grad_ms"] = [float(b) for _, b, _ in ms]
        res["wa_ms"] = [float(c) for _, _, c in ms]
    m = re.search(r"INTERP_TIME prepare_ms ([\d.]+)", err)
    if m:
        res["prepare_ms"] = float(m.group(1))
    return res


def message_verdict(cm, im, ce, ie):
    """identical, same_class (the same exception class) or differ."""
    if cm == im:
        return "identical"
    if ce and ce == ie:
        return "same_class"
    return "differ"


def grad_check(cg, ig, ierr):
    """(worst ratio of |difference| to the tolerance, unchecked count,
    largest relative gap |difference| / max(1, |compiled|) among the
    components outside the tolerance).

    A component is unchecked where the interpreter could not estimate its
    error (GRAD_ERR inf) and where only the compiled value is not finite:
    a difference quotient cannot see that a derivative does not exist.
    Without error estimates (an older stanli_check) the tolerance is
    relative, as for lp."""
    worst, unchecked, gap = 0.0, 0, 0.0
    for k, (a, b) in enumerate(zip(cg, ig)):
        a, b = float(a), float(b)
        e = float(ierr[k]) if ierr is not None and k < len(ierr) else None
        if e is not None and not math.isfinite(e):
            unchecked += 1
            continue
        if math.isnan(a) and math.isnan(b) or a == b:
            continue
        if e is not None and not math.isfinite(a) and math.isfinite(b):
            unchecked += 1
            continue
        if not (math.isfinite(a) and math.isfinite(b)):
            return math.inf, unchecked, math.inf
        if e is None:
            tol = max(ABS_TOL, TOLS[0] * max(abs(a), abs(b)))
        else:
            tol = GRAD_ERR_FACTOR * e + GRAD_REL_TOL * max(1.0, abs(a))
        worst = max(worst, abs(a - b) / tol)
        if abs(a - b) > tol:
            gap = max(gap, abs(a - b) / max(1.0, abs(a)))
    return worst, unchecked, gap


def compare(c, i):
    """Per quantity verdicts.

    lp and wa: agree_1e-10, agree_1e-8, disagree or n/a. grad: agree when
    every checked component is within the tolerance GRAD_ERR gives, else
    disagree. messages: identical, same_class or differ, over the failures
    both sides share. output: same or differ, over the printed lines."""
    v = {}
    if c["state"] == "compile_fail" and i["state"] == "compile_fail":
        return {"messages": message_verdict(c["message"], i["message"],
                                             c.get("error"), i.get("error")),
                "notes": [] if c["message"] == i["message"] else [
                    "compile: compiled %s / interp %s" % (
                        c["message"][:120], i["message"][:120])]}
    if c["state"] != "points" or i["state"] != "points":
        return v
    lp_w, g_w, wa_w = [], [], []
    g_bad, g_gap = [], 0.0
    shape = []
    msgs = []
    both_threw = one_threw = unchecked = components = 0
    for k, cp in c["points"].items():
        ip = i["points"].get(k, {"status": "none"})
        if cp["status"] == "ok" and ip["status"] == "ok":
            lp_w.append(worst_rel([cp["lp"]], [ip["lp"]]))
            if len(cp["grad"]) != len(ip["grad"]):
                shape.append(f"grad width {len(cp['grad'])} vs {len(ip['grad'])}")
                g_w.append(math.inf)
                g_bad.append(k)
            elif k in GRAD_POINTS and "grad_err" in ip:
                w, u, gap = grad_check(cp["grad"], ip["grad"], ip["grad_err"])
                g_w.append(w)
                unchecked += u
                components += len(cp["grad"])
                if w > 1:
                    g_bad.append(k)
                    g_gap = max(g_gap, gap)
            cw, iw = cp.get("wa"), ip.get("wa")
            # No write_array line and an empty row both mean no columns.
            if cw is None and iw and iw.get("names") == []:
                iw = None
            if iw is None and cw and cw.get("names") == []:
                cw = None
            if cw and iw and "names" in cw and "names" in iw:
                if cw["names"] != iw["names"]:
                    shape.append("wa names differ")
                    wa_w.append(math.inf)
                else:
                    wa_w.append(worst_rel(cw["values"], iw["values"]))
            elif cw or iw:
                if (cw or {}).get("threw") and (iw or {}).get("threw"):
                    m = message_verdict(cw["threw"], iw["threw"],
                                        cw.get("error"), iw.get("error"))
                    msgs.append(m)
                    if m != "identical":
                        shape.append("point %s wa: compiled %s / interp %s" % (
                            k, cw["threw"][:120], iw["threw"][:120]))
                else:
                    wa_w.append(math.inf)
                    shape.append("wa only one side: compiled=%s interp=%s" % (
                        "row" if cw and "names" in cw else (cw or {}).get("threw"),
                        "row" if iw and "names" in iw else (iw or {}).get("threw")))
        elif cp["status"] == "threw" and ip["status"] == "threw":
            both_threw += 1
            m = message_verdict(cp["message"], ip["message"], cp.get("error"),
                                ip.get("error"))
            msgs.append(m)
            if m != "identical":
                shape.append("point %s: compiled %s / interp %s" % (
                    k, cp["message"][:120], ip["message"][:120]))
        else:
            one_threw += 1
            shape.append("point %s: compiled %s / interp %s" % (
                k, cp.get("message", cp["status"]), ip.get("message", ip["status"])))
    cpr, ipr = c.get("prints", {}), i.get("prints", {})
    output = "same" if cpr == ipr else "differ"
    if output == "differ":
        for key in sorted(set(cpr) | set(ipr)):
            if cpr.get(key) != ipr.get(key):
                shape.append("output %s: compiled %s / interp %s" % (
                    key, (cpr.get(key) or [])[:2], (ipr.get(key) or [])[:2]))
                break

    def verdict(ws):
        if not ws:
            return "n/a"
        w = max(ws)
        if w <= TOLS[0]:
            return "agree_1e-10"
        if w <= TOLS[1]:
            return "agree_1e-8"
        return "disagree"
    order = ("identical", "same_class", "differ")
    v = {"lp": verdict(lp_w), "wa": verdict(wa_w),
         "grad": "n/a" if not g_w else (
             "agree" if not g_bad else
             "disagree" if all(k in g_bad for k in GRAD_POINTS) or
             any(k not in GRAD_POINTS for k in g_bad) else "one_point"),
         "grad_points_bad": g_bad,
         "grad_rel": g_gap,
         "messages": max(msgs, key=order.index) if msgs else "n/a",
         "output": output,
         "lp_worst": max(lp_w) if lp_w else None,
         "grad_worst": max(g_w) if g_w else None,
         "grad_unchecked": unchecked,
         "grad_components": components,
         "wa_worst": max(wa_w) if wa_w else None,
         "points_both_threw": both_threw, "points_one_threw": one_threw,
         "notes": shape[:4]}
    return v


def agrees(v, loose=False):
    """Do two sides agree on everything compare() looks at?"""
    vals = ("agree_1e-10", "agree_1e-8", "n/a") if loose else (
        "agree_1e-10", "n/a")
    return (not v.get("points_one_threw") and v.get("lp", "n/a") in vals
            and v.get("wa", "n/a") in vals
            and v.get("grad", "n/a") in ("agree", "n/a", "one_point")
            and v.get("messages", "n/a") in ("identical", "n/a")
            and v.get("output", "same") == "same")


def one(case, check, tmp, timeout, interp_o0=False, grad_max=GRAD_MAX):
    group, name, stan, data = case
    safe = re.sub(r"[^A-Za-z0-9_.-]", "_", f"{group}_{name}")
    mir = tmp / f"{safe}.mir"
    mir0 = tmp / f"{safe}.o0.mir"
    rec = {"corpus": group, "model": name, "stan": str(stan), "data": str(data)}
    e = run([check, "--emit-mir", stan], timeout)
    if e[0] != 0:
        rec["state"] = "stanc_fail"
        rec["message"] = (e[2] or e[1]).strip().splitlines()[:1]
        return rec
    mir.write_text(e[1])
    base = [check, stan, data, "--points", POINTS, "--wa-values"]

    def side(tag, mir_path, extra, env=None):
        status = tmp / f"{safe}.{tag}.status.jsonl"
        status.unlink(missing_ok=True)
        return summarize(run(base + ["--mir", mir_path, "--status-json", status]
                             + extra, timeout, env), status)

    # The gradient costs four evaluations per parameter: it is computed at
    # the two gradient points only, not for a model with more than
    # grad_max free parameters, and not for a solver model.
    genv = dict(os.environ, STANLI_INTERP_GRAD_POINTS=",".join(GRAD_POINTS),
                STANLI_INTERP_GRAD_MAX=str(grad_max))
    if uses_solver(rec):
        genv["STANLI_INTERP_NO_GRAD"] = "1"

    def interp(tag, mir_path):
        res = side(tag, mir_path, ["--interp"], genv)
        if res["state"] == "timeout":
            res = side(tag, mir_path, ["--interp"],
                       dict(os.environ, STANLI_INTERP_NO_GRAD="1"))
            res["no_gradient"] = True
        return res
    rec["compiled"] = side("compiled", mir, [])
    interp_mir = mir
    if interp_o0:
        e0 = run([check, "--emit-mir", stan, "--passes", "none"], timeout)
        if e0[0] == 0:
            mir0.write_text(e0[1])
            interp_mir = mir0
            rec["interp_mir"] = "none"
    rec["interp"] = interp("interp", interp_mir)
    rec["cmp"] = compare(rec["compiled"], rec["interp"])
    if interp_mir is mir0 and not agrees(rec["cmp"], loose=True):
        rec["interp_shipping"] = interp("interp_shipping", mir)
        rec["cmp_shipping"] = compare(rec["compiled"], rec["interp_shipping"])
        rec["blame"] = ("stanc3_pass" if agrees(rec["cmp_shipping"], loose=True)
                        else "runtime")
    rec["state"] = classify(rec)
    return rec


def first_fail(side):
    if side["state"] in ("compile_fail", "crash"):
        return side.get("message", side["state"])
    if side["state"] in ("timeout", "memory"):
        return side.get("message", side["state"])
    for p in side.get("points", {}).values():
        if p["status"] == "threw":
            return p["message"]
    return None


# An adaptive solver's result on doubles differs from CmdStan's on vars
# (which also solves for the sensitivities) by about its tolerance, and a
# difference quotient of it changes with the solver's steps.
SOLVERS = re.compile(r"\b(ode_\w+|integrate_ode\w*|integrate_1d|dae\w*|"
                     r"algebra_solver\w*|solve_newton\w*|solve_powell\w*)"
                     r"\s*\(")


def uses_solver(rec):
    try:
        return bool(SOLVERS.search(pathlib.Path(rec["stan"]).read_text()))
    except OSError:
        return False


def classify(rec):
    c, i, v = rec["compiled"], rec["interp"], rec["cmp"]
    if c["state"] == "compile_fail" and i["state"] == "compile_fail":
        if v["messages"] != "identical" and c.get("error") in \
                check_status.UNSUPPORTED and i.get("error") in \
                check_status.UNSUPPORTED:
            return "both_unsupported"
        return "both_compile_fail_" + v["messages"]
    if c["state"] != "points":
        return "compiled_refused" if i["state"] == "points" else "both_refused"
    if i["state"] != "points":
        return "interp_refused"
    if v.get("points_one_threw"):
        cfail = any(p["status"] == "threw" for p in c["points"].values())
        ifail = any(p["status"] == "threw" for p in i["points"].values())
        if ifail and not cfail:
            return "interp_threw"
        if cfail and not ifail:
            return "compiled_threw"
        return "one_side_threw"
    if not all(v[q] in ("agree_1e-10", "agree_1e-8", "n/a")
               for q in ("lp", "wa")):
        return "solver_disagree" if uses_solver(rec) else "disagree"
    if v["messages"] not in ("identical", "n/a"):
        return "message_" + v["messages"]
    if v["output"] != "same":
        return "output_differs"
    if v["grad"] == "disagree":
        return "gradient_only"
    if v["lp"] == "n/a":
        return "both_threw_everywhere"
    if all(v[q] in ("agree_1e-10", "n/a") for q in ("lp", "wa")):
        return "agree_1e-10"
    return "agree_1e-8"


def error_key(msg):
    if not msg:
        return "?"
    msg = msg.split(" | in: ")[0]
    msg = re.sub(r"\d+(\.\d+)?(e[-+]?\d+)?", "N", msg)
    return msg[:120]


def table(records):
    by = collections.defaultdict(list)
    for r in records:
        by[r["corpus"]].append(r)
    lines = []
    for corpus, rs in sorted(by.items()):
        cnt = collections.Counter(r["state"] for r in rs)
        q = {k: collections.Counter(r.get("cmp", {}).get(k, "n/a") for r in rs)
             for k in ("lp", "grad", "wa", "messages", "output")}
        lines.append(f"## {corpus}: {len(rs)} models")
        lines.append("  states: " + ", ".join(f"{k}={n}" for k, n in cnt.most_common()))
        for k in ("lp", "grad", "wa", "messages", "output"):
            lines.append(f"  {k}: " + ", ".join(f"{a}={n}" for a, n in sorted(q[k].items())))
        groups = collections.defaultdict(list)
        for r in rs:
            if r["state"] in ("interp_refused", "interp_threw"):
                groups[error_key(first_fail(r["interp"]))].append(r["model"])
        for key, ms in sorted(groups.items(), key=lambda kv: -len(kv[1])):
            lines.append(f"  interp fails x{len(ms)}: {key}  e.g. {', '.join(ms[:2])}")
        cgroups = collections.defaultdict(list)
        for r in rs:
            if r["state"] in ("compiled_refused", "compiled_threw"):
                cgroups[error_key(first_fail(r["compiled"]))].append(r["model"])
        for key, ms in sorted(cgroups.items(), key=lambda kv: -len(kv[1])):
            lines.append(f"  compiled fails x{len(ms)}: {key}  e.g. {', '.join(ms[:2])}")
        dis = [r for r in rs if r["state"] in ("disagree", "solver_disagree",
                                               "gradient_only")
               or r["state"].startswith(("message_", "output_",
                                         "both_compile_fail_s",
                                         "both_compile_fail_d"))]
        for r in dis[:40]:
            v = r["cmp"]
            lines.append(f"  {r['state']} {r['model']}: lp={v.get('lp_worst')} "
                         f"grad_rel={v.get('grad_rel')} at {v.get('grad_points_bad')} "
                         f"wa={v.get('wa_worst')} "
                         f"{v.get('notes', [])[:2]}")
        partly = [r["model"] for r in rs
                  if r.get("cmp", {}).get("grad_unchecked")]
        none = [r["model"] for r in rs if r.get("cmp", {}).get("grad_components")
                and r["cmp"]["grad_unchecked"] == r["cmp"]["grad_components"]]
        nograd = [r["model"] for r in rs if r.get("interp", {}).get("no_gradient")]
        if nograd:
            lines.append(f"  interp gradient timed out, compared without it: "
                         f"{len(nograd)}: {', '.join(nograd[:6])}")
        if partly:
            lines.append(f"  grad components unchecked (GRAD_ERR inf): "
                         f"{len(partly)} models, {len(none)} with none "
                         f"checked; e.g. {', '.join((none or partly)[:4])}")
        blame = collections.Counter(r["blame"] for r in rs if "blame" in r)
        if blame:
            lines.append("  interp on --passes none MIR disagrees; on shipping "
                         "MIR: " + ", ".join(f"{k}={n}" for k, n in
                                             blame.most_common()))
        ct = [r["compiled"]["wall"] for r in rs if r.get("compiled", {}).get("state") == "points"]
        it = [r["interp"]["wall"] for r in rs if r.get("interp", {}).get("state") == "points"
              and r.get("compiled", {}).get("state") == "points"]
        if ct and it:
            ct.sort(), it.sort()
            lines.append(f"  wall s (median/max): compiled {ct[len(ct)//2]:.3f}/{ct[-1]:.2f}, interp {it[len(it)//2]:.3f}/{it[-1]:.2f}")
            pairs = [(r["interp"]["wall"], r["compiled"]["wall"], r["model"]) for r in rs
                     if r.get("interp", {}).get("state") == "points" and r.get("compiled", {}).get("state") == "points"]
            slow = sorted(pairs, key=lambda p: -p[0] / max(p[1], 1e-3))[:3]
            lines.append("  slowest interp/compiled: " + ", ".join(
                f"{m} {a:.2f}s vs {b:.2f}s" for a, b, m in slow))
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=pathlib.Path, required=True)
    ap.add_argument("--corpus", action="append",
                    default=[])
    ap.add_argument("--check", type=pathlib.Path,
                    default=REPO / "build-rel" / "stanli_check")
    ap.add_argument("--pdb", type=pathlib.Path, default=REPO / "deps" / "posteriordb")
    ap.add_argument("--jobs", type=int, default=24)
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--filter", default="")
    ap.add_argument("--grad-max", type=int, default=GRAD_MAX,
                    help="skip the gradient for models with more free "
                    "parameters")
    ap.add_argument("--interp-o0", action="store_true",
                    help="give the interpreter the --passes none MIR")
    ap.add_argument("--report", action="store_true",
                    help="only print the table of the JSON files in --out")
    args = ap.parse_args()
    if args.report:
        records = []
        for f in sorted(args.out.glob("*.json")):
            records += json.loads(f.read_text())
        for r in records:
            if "compiled" in r and "interp" in r:
                r["cmp"] = compare(r["compiled"], r["interp"])
                r["state"] = classify(r)
        text = table(records)
        (args.out / "table.txt").write_text(text + "\n")
        print(text)
        return
    corpora = args.corpus or ["fixtures", "lit", "stanc3_good", "posteriordb",
                              "brms", "rethinking", "cogmod", "other_local"]
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="interp_gap_") as t:
        tmp = pathlib.Path(t)
        (tmp / "empty.json").write_text("{}")
        cs = [c for c in cases(corpora, args.pdb, tmp) if args.filter in c[1]]
        print(f"{len(cs)} models", flush=True)
        records = []
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            futs = [pool.submit(one, c, args.check, tmp, args.timeout,
                                args.interp_o0, args.grad_max) for c in cs]
            for n, f in enumerate(concurrent.futures.as_completed(futs), 1):
                records.append(f.result())
                if n % 100 == 0:
                    print(f"  {n}/{len(cs)}", flush=True)
    by = collections.defaultdict(list)
    for r in records:
        by[r["corpus"]].append(r)
    for corpus, rs in by.items():
        rs.sort(key=lambda r: r["model"])
        (args.out / f"{corpus}.json").write_text(json.dumps(rs, indent=1))
    text = table(records)
    (args.out / "table.txt").write_text(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
