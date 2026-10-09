#!/usr/bin/env python3
import collections
import json
import pathlib
import re
import sys

OPS = re.compile(r"^[A-Z][A-Za-z]*__$")
PROB = re.compile(r"_(lpdf|lpmf|lupdf|lupmf|cdf|lcdf|lccdf|cdf_log|ccdf_log|log)$")


def norm(n):
    return re.sub(r"_lu(p[dm]f)$", r"_l\1", n)


def kind(f):
    if f.startswith("user:"):
        return "user"
    if OPS.match(f):
        return "operator"
    if PROB.search(f):
        return "density"
    return "function"


data = json.load(open(sys.argv[1]))
models = {n: m for n, m in data["models"].items() if m}
want_gq = len(sys.argv) > 2 and sys.argv[2] == "gq"
secs = ("prepare_data", "log_prob", "functions_block") + (("generate_quantities",) if want_gq else ())
fm, cm = {}, {}
for n, m in models.items():
    f, c = set(), set()
    for s in secs:
        for k in m[s]["funcs"]:
            if not k.startswith("user:"):
                f.add(norm(k))
        c.update(m[s]["constrain"])
    fm[n], cm[n] = f, c
print(len(models), "models; sections", secs)
cnt = collections.Counter(x for f in fm.values() for x in f)
for k in ("operator", "density", "function"):
    names = [x for x in cnt if kind(x) == k]
    print(k, len(names))
print("constraints", len(set(x for c in cm.values() for x in c)))
for k, v in collections.Counter(x for c in cm.values() for x in c).most_common():
    print("  ", k, v)
print("densities")
for k, v in cnt.most_common():
    if kind(k) == "density":
        print("  ", k, v)


def coverage(order, label, sets):
    rank = {f: i for i, f in enumerate(order)}
    need = {n: max((rank[f] for f in s), default=-1) for n, s in sets.items()}
    pts = [5, 10, 20, 30, 40, 60, 80, 100, 120, 150, 200, len(order)]
    print(label)
    for K in pts:
        print("   top", K, "->", sum(1 for v in need.values() if v < K), "models")


allf = {n: fm[n] | {"C:" + x for x in cm[n]} for n in fm}
cnt2 = collections.Counter(x for s in allf.values() for x in s)
order = [k for k, _ in cnt2.most_common()]
print("distinct items (functions+operators+constraints):", len(order))
coverage(order, "frequency order, functions+operators+densities+constraints", allf)

nonop = {n: {x for x in s if kind(x) != "operator"} if not x.startswith("C:") else x for n, s in allf.items() for x in [None]} if False else None

greedy = []
remaining = dict(allf)
done = set()
while remaining and len(greedy) < len(order):
    best, bs = None, -1
    for f in order:
        if f in done:
            continue
        score = sum(1 for n, s in remaining.items() if f in s and (s - done - {f}) == set())
        score2 = sum(1 for n, s in remaining.items() if f in s) * 1e-3
        if score + score2 > bs:
            best, bs = f, score + score2
    greedy.append(best)
    done.add(best)
    remaining = {n: s for n, s in remaining.items() if not s <= done}
print("greedy order head:", greedy[:15])
coverage(greedy, "greedy order (maximise newly complete models)", allf)
blocked = collections.Counter()
for n, s in allf.items():
    miss = s - set(order[:60])
    if len(miss) == 1:
        blocked[next(iter(miss))] += 1
print("sole blockers beyond top 60:", blocked.most_common(15))
print("function frequency top 60")
print(", ".join(f"{k}:{v}" for k, v in cnt2.most_common(60)))
byc = collections.defaultdict(list)
col = data["collection"]
for n in models:
    byc[col[n]].append(len(allf[n]))
for c, v in sorted(byc.items()):
    print("collection", c, len(v), "median items", sorted(v)[len(v) // 2])
print("tail (count<=2):", sum(1 for v in cnt2.values() if v <= 2))
print("rare list:", ", ".join(k for k, v in cnt2.items() if v <= 2))
