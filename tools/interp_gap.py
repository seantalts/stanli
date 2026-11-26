#!/usr/bin/env python3
"""How far is `stanli_check --interp` from the compiled path?

For each model this emits the shipping MIR once (and the `--passes none`
MIR), then runs stanli_check on it twice at the same points: once through
the compiled path and once with --interp, which runs the whole model through
the MIR interpreter. It compares lp, the gradient and the write_array row,
and writes one JSON file per corpus plus a table.

    tools/interp_gap.py --out DIR [--corpus NAME ...] [--jobs N]

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
import pathlib
import re
import subprocess
import sys
import tempfile
import threading
import time

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "tools" / "fuzz_stanc"))
from corpus_inventory import corpus_cases, materialize_data  # noqa: E402
import check_stanc3_good as good  # noqa: E402

POINTS = "0,1,2"
TOLS = (1e-10, 1e-8)
ABS_TOL = 1e-12


def close(a, b, rel):
    a, b = float(a), float(b)
    if math.isnan(a) or math.isnan(b):
        return math.isnan(a) and math.isnan(b)
    if math.isinf(a) or math.isinf(b):
        return a == b
    return abs(a - b) <= max(ABS_TOL, rel * max(abs(a), abs(b)))


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


def run(argv, timeout):
    """(code, stdout, stderr, wall); code None on timeout, "rss" when the
    process passed MAX_RSS_KB and was killed."""
    t = time.perf_counter()
    p = subprocess.Popen([str(a) for a in argv], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, errors="replace")
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


def summarize(proc):
    code, out, err, wall = proc
    if code is None:
        return {"state": "timeout", "wall": wall}
    if code == "rss":
        return {"state": "memory", "wall": wall,
                "message": f"killed above {MAX_RSS_KB // 1024 // 1024} GB RSS"}
    parsed = good.parse_output(out)
    pre = parsed["pre"]
    res = {"wall": wall, "code": code}
    if pre and pre[0] == "COMPILE_FAIL":
        res.update(state="compile_fail", message=pre[1][:300])
        return res
    if not parsed["points"] or code not in (0, 1):
        tail = (err or out).strip().splitlines()[-1:] or [f"exit {code}"]
        res.update(state="crash", message=tail[0][:300])
        return res
    pts = {}
    for k, sec in parsed["points"].items():
        st = sec["status"]
        p = {}
        if st is None:
            p["status"] = "none"
        elif st[0] == "OK":
            p["status"] = "ok"
            p["lp"] = st[1][0]
            p["grad"] = st[1][1:]
        else:
            p["status"] = "threw"
            p["message"] = st[1][:300]
        wa = sec["wa"]
        if wa and wa[0] == "ok":
            p["wa"] = {"names": wa[1], "values": wa[2]}
        elif wa:
            p["wa"] = {"threw": wa[1][:300]}
        pts[str(k)] = p
    res.update(state="points", points=pts)
    ms = re.findall(r"INTERP_TIME point \d+ lp_ms ([\d.]+) wa_ms ([\d.]+)", err)
    if ms:
        res["lp_ms"] = [float(a) for a, _ in ms]
        res["wa_ms"] = [float(b) for _, b in ms]
    m = re.search(r"INTERP_TIME prepare_ms ([\d.]+)", err)
    if m:
        res["prepare_ms"] = float(m.group(1))
    return res


def compare(c, i):
    """Per quantity verdicts: agree_1e-10, agree_1e-8, disagree, or n/a."""
    v = {}
    if c["state"] != "points" or i["state"] != "points":
        return v
    lp_w, g_w, wa_w = [], [], []
    shape = []
    both_threw = one_threw = 0
    for k, cp in c["points"].items():
        ip = i["points"].get(k, {"status": "none"})
        if cp["status"] == "ok" and ip["status"] == "ok":
            lp_w.append(worst_rel([cp["lp"]], [ip["lp"]]))
            if len(cp["grad"]) != len(ip["grad"]):
                shape.append(f"grad width {len(cp['grad'])} vs {len(ip['grad'])}")
                g_w.append(math.inf)
            else:
                g_w.append(worst_rel(cp["grad"], ip["grad"]))
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
                    pass
                else:
                    wa_w.append(math.inf)
                    shape.append("wa only one side: compiled=%s interp=%s" % (
                        "row" if cw and "names" in cw else (cw or {}).get("threw"),
                        "row" if iw and "names" in iw else (iw or {}).get("threw")))
        elif cp["status"] == "threw" and ip["status"] == "threw":
            both_threw += 1
        else:
            one_threw += 1
            shape.append("point %s: compiled %s / interp %s" % (
                k, cp.get("message", cp["status"]), ip.get("message", ip["status"])))

    def verdict(ws):
        if not ws:
            return "n/a"
        w = max(ws)
        if w <= TOLS[0]:
            return "agree_1e-10"
        if w <= TOLS[1]:
            return "agree_1e-8"
        return "disagree"
    v = {"lp": verdict(lp_w), "grad": verdict(g_w), "wa": verdict(wa_w),
         "lp_worst": max(lp_w) if lp_w else None,
         "grad_worst": max(g_w) if g_w else None,
         "wa_worst": max(wa_w) if wa_w else None,
         "points_both_threw": both_threw, "points_one_threw": one_threw,
         "notes": shape[:4]}
    return v


def one(case, check, tmp, timeout):
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
    e0 = run([check, "--emit-mir", stan, "--passes", "none"], timeout)
    if e0[0] == 0:
        mir0.write_text(e0[1])
    base = [check, stan, data, "--points", POINTS, "--wa-values"]
    rec["compiled"] = summarize(run(base + ["--mir", mir], timeout))
    rec["interp"] = summarize(run(base + ["--mir", mir, "--interp"], timeout))
    rec["cmp"] = compare(rec["compiled"], rec["interp"])
    if e0[0] == 0:
        rec["interp_o0"] = summarize(run(base + ["--mir", mir0, "--interp"],
                                         timeout))
        rec["cmp_o0"] = compare(rec["compiled"], rec["interp_o0"])
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


def classify(rec):
    c, i, v = rec["compiled"], rec["interp"], rec["cmp"]
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
    if all(v[q] in ("agree_1e-10", "n/a") for q in ("lp", "grad", "wa")):
        if v["lp"] == "n/a":
            return "both_threw_everywhere"
        return "agree_1e-10"
    if all(v[q] in ("agree_1e-10", "agree_1e-8", "n/a")
           for q in ("lp", "grad", "wa")):
        return "agree_1e-8"
    return "disagree"


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
             for k in ("lp", "grad", "wa")}
        lines.append(f"## {corpus}: {len(rs)} models")
        lines.append("  states: " + ", ".join(f"{k}={n}" for k, n in cnt.most_common()))
        for k in ("lp", "grad", "wa"):
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
        dis = [r for r in rs if r["state"] == "disagree"]
        for r in dis[:12]:
            v = r["cmp"]
            lines.append(f"  disagree {r['model']}: lp={v['lp_worst']} grad={v['grad_worst']} wa={v['wa_worst']} {v['notes'][:2]}")
        o0 = collections.Counter(r["state"] for r in rs)  # placeholder
        o0 = collections.Counter(
            ("ok" if r.get("cmp_o0") and all(
                r["cmp_o0"].get(k) in ("agree_1e-10", "agree_1e-8", "n/a")
                for k in ("lp", "grad", "wa")) and r["interp_o0"]["state"] == "points"
             and not r["cmp_o0"].get("points_one_threw") else
             (r.get("interp_o0") or {}).get("state", "no_o0"))
            for r in rs if r["state"] not in ("stanc_fail", "compiled_refused", "both_refused"))
        lines.append("  interp on --passes none MIR vs compiled: " + ", ".join(
            f"{k}={n}" for k, n in o0.most_common()))
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
    ap.add_argument("--report", action="store_true",
                    help="only print the table of the JSON files in --out")
    args = ap.parse_args()
    if args.report:
        records = []
        for f in sorted(args.out.glob("*.json")):
            records += json.loads(f.read_text())
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
            futs = [pool.submit(one, c, args.check, tmp, args.timeout) for c in cs]
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
