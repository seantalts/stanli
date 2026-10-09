#!/usr/bin/env python3
"""THROWAWAY (spike/vector-libm). Summarize analyze_prof.py output.
summarize_prof.py profile.json profile_status.json"""
import collections
import json
import statistics
import sys

r = json.load(open(sys.argv[1]))
status = json.load(open(sys.argv[2]))
G = ("elem", "gamma", "boost_special", "longdouble", "fma", "libm_misc")
TR = ("elem", "gamma", "boost_special", "longdouble")
rows = []
for m, v in r.items():
    if status.get(m, {}).get("status") != "ok" or v["n"] < 500:
        print("skipped", m, v["n"])
        continue
    rows.append((m, v["n"], {g: v["groups"].get(g, 0) / v["n"] for g in G}))
N = len(rows)
print("models", N)


def dist(label, xs):
    q = statistics.quantiles(xs, n=20)
    print("%-48s mean %.3f  p25 %.3f  median %.3f  p75 %.3f  p90 %.3f  p95 %.3f  max %.3f | >=10%%: %d  >=30%%: %d  >=50%%: %d" % (
        label, statistics.mean(xs), q[4], statistics.median(xs), q[14], q[17], q[18], max(xs),
        sum(x >= .1 for x in xs), sum(x >= .3 for x in xs), sum(x >= .5 for x in xs)))


dist("all transcendental (elem+gamma+boost+longdouble)", [sum(g[k] for k in TR) for _, _, g in rows])
for g in G:
    dist(g, [x[2][g] for x in rows])
agg = collections.Counter()
cnt = collections.Counter()
top = {}
sagg = collections.Counter()
scnt = collections.Counter()
stop = collections.defaultdict(list)
for m, n, g in rows:
    for fn, c in r[m]["fns"].items():
        agg[fn] += c / n
        cnt[fn] += c / n >= 0.05
        if c / n > top.get(fn, (0, ""))[0]:
            top[fn] = (c / n, m)
    per_site = collections.Counter()
    for s, c in r[m]["sites"].items():
        if s.split("/")[0] in ("fma", "libm_misc"):
            continue
        per_site[s.split(" @ ")[1]] += c / n
    for s, c in per_site.items():
        sagg[s] += c
        scnt[s] += c >= 0.05
        stop[s].append((c, m))
print("\nfunction: mean share over models, #models >=5%, top model")
for fn, a in agg.most_common(28):
    print("  %-40s %.4f  %3d  %.2f %s" % (fn, a / N, cnt[fn], top[fn][0], top[fn][1]))
print("\nsite (kernel or Stan Math density holding the call): mean transcendental share, #models >=5%, top models")
for s, a in sagg.most_common(45):
    t = sorted(stop[s], reverse=True)[:4]
    print("  %-52s %.4f  %3d  %s" % (s[-52:], a / N, scnt[s], ", ".join("%s %.2f" % (m, c) for c, m in t)))
print("\ntop models by transcendental share")
rows.sort(key=lambda x: -sum(x[2][k] for k in TR))
for m, n, g in rows[:30]:
    fns = sorted(r[m]["fns"].items(), key=lambda kv: -kv[1])[:4]
    print("  %.3f (elem %.2f gamma %.2f) %-28s %s" % (
        sum(g[k] for k in TR), g["elem"], g["gamma"], m,
        ", ".join("%s %.2f" % (f.split("/")[1], c / n) for f, c in fns)))

print("\ntranscendental share by width of the op being run (widest input/output length of the kernel call)")
WC = ("w1", "w2-3", "w4-15", "w16+", "w0", "w?")
tot = collections.Counter()
per_model = []
for m, n, g in rows:
    w = r[m].get("widths", {})
    d = {c: sum(v for k, v in w.items() if k.split("/")[0] in TR and k.endswith("/" + c)) / n for c in WC}
    e = {c: sum(v for k, v in w.items() if k.split("/")[0] == "elem" and k.endswith("/" + c)) / n for c in WC}
    per_model.append((m, d, e))
for c in WC:
    xs = [d[c] for _, d, _ in per_model]
    es = [e[c] for _, _, e in per_model]
    print("  %-6s all transcendental: mean %.3f median %.3f | elementary only: mean %.3f median %.3f" % (
        c, statistics.mean(xs), statistics.median(xs), statistics.mean(es), statistics.median(es)))
dist("elementary in ops of width >= 4", [e["w4-15"] + e["w16+"] for _, _, e in per_model])
dist("elementary in ops of width >= 16", [e["w16+"] for _, _, e in per_model])
dist("elementary in ops of width 1", [e["w1"] for _, _, e in per_model])
dist("all transcendental in ops of width >= 4", [d["w4-15"] + d["w16+"] for _, d, _ in per_model])
