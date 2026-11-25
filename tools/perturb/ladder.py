"""The scaling stage: sweep each size variable of a model over a ladder.

The source stays the same and scale.py writes the data for each size. Every
point is checked and prepared in parallel across models; the gradients are
then timed one process at a time. The detectors at the bottom turn a ladder
into findings, and are plain functions of the recorded points.
"""
from __future__ import annotations

import concurrent.futures
import json
import math
import pathlib
import time

import measure as m
import scale

LADDER = (0, 1, 2, 3, 7, 8, 9, 31, 32, 33, 63, 64, 65, 100, 1000, 10**4, 10**5)
# The caps that lowering applies (runtime/src/lower.cpp): the program
# budgets on the log-density program's bytes, and the unroll ceiling on the
# ops one loop lowers to before the passes.
BYTE_CAPS = {"program budget 8 MiB": 8 << 20, "loop budget 32 MiB": 32 << 20}
UNROLL_CEILING_OPS = 1 << 20
CAP_MARGIN = 0.03
GROW_SLOPE = 0.5
PREP_EXPONENT = 1.15
PREP_FLOOR_S = 0.05
RSS_FLOOR = 50 << 20
JUMP = 1.3
BISECT_STEPS = 14
PAIRS = 6
MAX_DATA_BYTES = 400 << 20
MAX_FIRST_GRADIENT_S = 5.0
REJECTED = ("data", "model")  # the model or its data check rejects the size


# ---------------------------------------------------------------------------
# Sources


def sources(root, only=None):
    """[(name, stan, data, {var: sizes} or None)]: each seed's base over
    every size variable, and each variant whose meta names "sizes"."""
    out = []
    for seed in sorted(p for p in pathlib.Path(root).iterdir()
                       if (p / "base.stan").exists()):
        out.append((seed.name, seed / "base.stan", seed / "base.json", None))
        for meta_path in sorted(seed.glob("*.meta.json")):
            meta = json.loads(meta_path.read_text())
            if not meta.get("sizes"):
                continue
            name = meta_path.name[:-len(".meta.json")]
            data = seed / f"{name}.json"
            out.append((f"{seed.name}/{name}", seed / f"{name}.stan",
                        data if data.exists() else seed / "base.json",
                        meta["sizes"]))
    if only:
        out = [s for s in out if any(o in s[0] for o in only)]
    return out


# ---------------------------------------------------------------------------
# One point


def classify_check(outcome, records):
    """(status, message): ok, rejected (the model or its data checks refuse
    this size: not a finding), or failure."""
    state, payload = outcome
    if state == "compile_fail":
        stage = (records.compile or {}).get("stage")
        if stage in REJECTED:
            return "rejected", str(payload)[:300]
        return "failure", f"compile: {payload}"[:300]
    if state in ("crash", "timeout", "refused"):
        return "failure", f"{state}: {payload}"[:300]
    why = m.finite_values(outcome, points=(0,))
    if why:
        if "EVAL_FAIL" in why or "non-finite" in why:
            return "rejected", why
        return "failure", why
    return "ok", None


def measure_point(tools, comp, data_path, size):
    point = {"size": size, "data": str(data_path)}
    outcome, records = m.check(tools, comp, data_path, points=(0,))
    point["status"], point["message"] = classify_check(outcome, records)
    if point["status"] != "ok":
        return point
    s = m.structure(tools, comp, data_path, prep_reps=1)
    if s.get("failure"):
        point["status"], point["message"] = "failure", s["failure"]
        return point
    point.update({k: s.get(k) for k in (
        "ops", "lowered_ops", "wa_ops", "wa_lowered_ops", "program_bytes",
        "executor_bytes", "slot_elems", "prep_s", "peak_rss",
        "first_gradient_s", "declines", "lowering", "wa_lowering", "islands")})
    return point


def predicted_stop(points, size, data_bytes, args):
    """Why the next size would pass a limit, judged from the last good
    point, or None."""
    good = [p for p in points if p["status"] == "ok" and p["size"] > 0]
    if not good:
        return None
    last = good[-1]
    grow = size / last["size"]
    if last["prep_s"] * grow > args.max_seconds:
        return f"preparation predicted over {args.max_seconds:.0f} s"
    if last["peak_rss"] * grow > args.max_rss_gb * 2**30:
        return f"peak RSS predicted over {args.max_rss_gb:.0f} GB"
    if data_bytes > MAX_DATA_BYTES:
        return f"data over {MAX_DATA_BYTES >> 20} MB"
    return None


def cap_sizes(points):
    """Sizes just below and above where the measured program would cross
    each lowering cap, from the growth between the two largest points that
    are still under it. Empty when the program does not grow."""
    good = [p for p in points if p["status"] == "ok" and p["size"] >= 2
            and p.get("program_bytes")]
    out = set()
    for what, cap, key in [(k, v, "program_bytes") for k, v in BYTE_CAPS.items()] \
            + [("unroll ceiling", UNROLL_CEILING_OPS, "lowered_ops")]:
        under = [p for p in good if p.get(key) and p[key] < cap]
        if len(under) < 2:
            continue
        a, b = under[-2], under[-1]
        if b[key] <= a[key]:
            continue
        slope = math.log(b[key] / a[key]) / math.log(b["size"] / a["size"])
        if slope < GROW_SLOPE:
            continue
        cross = b["size"] * (cap / b[key]) ** (1 / slope)
        for f in (1 - CAP_MARGIN, 1 + CAP_MARGIN):
            out.add((max(1, round(cross * f)), what))
    return sorted(out)


def ops_jumps(points):
    """Adjacent good sizes between which the log-density op count more
    than doubles, with nothing measured between them."""
    pts = [p for p in points if p["status"] == "ok" and p.get("ops")]
    return [(a["size"], b["size"]) for a, b in zip(pts, pts[1:])
            if b["size"] - a["size"] > 1 and b["ops"] > 2 * a["ops"]
            and b["ops"] - a["ops"] >= 100]


def run_ladder(tools, comp, text, data, var, sizes, work, args):
    lad = {"points": [], "stop": ""}

    def at(size):
        """Measure one size unless it is done or past a limit; the point."""
        done = next((p for p in lad["points"] if p["size"] == size), None)
        if done or lad["stop"]:
            return done
        try:
            new, info = scale.scale(text, data, var, size)
        except scale.Refused as e:
            lad["points"].append({"size": size, "status": "refused",
                                  "message": str(e)})
            return lad["points"][-1]
        blob = json.dumps(new)
        stop = predicted_stop(lad["points"], size, len(blob), args)
        if stop:
            lad["stop"] = f"stopped before {size}: {stop}"
            return None
        path = work / f"{var}-{size}.json"
        path.write_text(blob)
        point = measure_point(tools, comp, path, size)
        point["scaled"] = info
        lad["points"].append(point)
        lad["points"].sort(key=lambda p: p["size"])
        return point

    for size in sizes:
        at(size)
    stop, lad["stop"] = lad["stop"], ""
    lad["cap_points"] = []
    for size, why in cap_sizes(lad["points"]):
        if size < max(sizes) * 4:
            lad["cap_points"].append({"size": size, "cap": why})
            at(size)
    lad["stop"] = lad["stop"] or stop
    # Locate each jump in the op count to one size, by bisection.
    lad["thresholds"] = []
    for lo, hi in ops_jumps(lad["points"]):
        ops_lo = next(p["ops"] for p in lad["points"] if p["size"] == lo)
        for _ in range(BISECT_STEPS):
            if hi - lo <= 1:
                break
            mid = (lo + hi) // 2 if hi < 4 * lo else round(math.sqrt(lo * hi))
            p = at(mid)
            if p is None or p["status"] != "ok":
                break
            if p["ops"] > 2 * ops_lo:
                hi = mid
            else:
                lo = mid
        lad["thresholds"].append([lo, hi])
    return lad


def ladder_sizes(sv, wanted):
    lower = sv.lower if sv.lower is not None else 1
    sizes = wanted or LADDER
    return [s for s in sorted(set(int(x) for x in sizes)) if s >= max(lower, 0)]


def structural(tools, source, args):
    name, stan, data_path, wanted = source
    started = time.monotonic()
    text = stan.read_text()
    data = json.loads(data_path.read_text())
    x = {"name": name, "ladders": {}, "refused": {}}
    comp = m.compile_source(tools, text)
    x["compile_s"] = comp["seconds"]
    if not comp["ok"]:
        x["error"] = f"does not compile: {comp['error'][:200]}"
        return x
    try:
        found = scale.size_variables(text, data)
    except scale.Refused as e:
        x["error"] = f"scale.py: {e}"
        return x
    work = tools.cache.parent / "scaling" / name.replace("/", "__")
    work.mkdir(parents=True, exist_ok=True)
    for var, sv in sorted(found.items()):
        if wanted is not None and var not in wanted:
            continue
        if sv.refused:
            x["refused"][var] = sv.refused
            continue
        lad = run_ladder(tools, comp, text, data, var,
                         ladder_sizes(sv, (wanted or {}).get(var)), work, args)
        lad.update(kind=sv.kind, value=sv.value, inferred=sv.inferred)
        x["ladders"][var] = lad
    x["structure_s"] = time.monotonic() - started
    return x


# ---------------------------------------------------------------------------
# Timing


def time_ladders(tools, x, args, meta):
    started = time.monotonic()
    comp = m.compile_source(tools, pathlib.Path(x["stan"]).read_text())
    for var, lad in x["ladders"].items():
        for p in lad["points"]:
            if p["status"] != "ok" or p["size"] == 0:
                continue
            if (p.get("first_gradient_s") or 0) > MAX_FIRST_GRADIENT_S:
                p["timing"] = "skipped: first gradient over 5 s"
                continue
            meta["load_1min"].append(m.load_average())
            try:
                t = m.time_once(m.stanli_cmd(tools, comp, p["data"], None),
                                tools.timeout)
                p["ns"] = sum(t.values()) / len(t)
            except RuntimeError as e:
                p["timing"] = str(e)
    x["timing_s"] = time.monotonic() - started


# ---------------------------------------------------------------------------
# Detectors


def slope(points, key):
    """Least-squares slope of log(key) on log(size)."""
    xs = [math.log(p["size"]) for p in points]
    ys = [math.log(p[key]) for p in points]
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    den = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den if den else 0.0


def ops_grow(var, points):
    out = []
    for key, graph in (("ops", "log_prob"), ("wa_ops", "write_array")):
        pts = [p for p in points if p["status"] == "ok" and p["size"] >= 8
               and p.get(key)]
        if len(pts) < 3:
            continue
        s = slope(pts, key)
        if s >= GROW_SLOPE and pts[-1][key] >= 2 * pts[0][key]:
            out.append({"kind": "ops_grow", "var": var, "graph": graph,
                        "slope": s, "sizes": f"{pts[0]['size']} -> {pts[-1]['size']}",
                        "detail": f"{graph} ops {pts[0][key]} -> {pts[-1][key]}, "
                                  f"log-log slope {s:.2f}",
                        "pair": [pts[0]["size"], pts[-1]["size"]]})
    return out


def exponent(a, b, key, base=0.0):
    da, db = a[key] - base, b[key] - base
    if da <= 0 or db <= 0:
        return None
    return math.log(db / da) / math.log(b["size"] / a["size"])


def prep_superlinear(var, points):
    pts = [p for p in points if p["status"] == "ok" and p["size"] > 0
           and p.get("prep_s") is not None]
    if len(pts) < 3:
        return []
    a, b = pts[-2], pts[-1]
    out = []
    e = exponent(a, b, "prep_s")
    if e is not None and e > PREP_EXPONENT and b["prep_s"] >= PREP_FLOOR_S:
        out.append({"kind": "prep_superlinear", "var": var,
                    "sizes": f"{a['size']} -> {b['size']}",
                    "detail": f"preparation {a['prep_s'] * 1e3:.1f} -> "
                              f"{b['prep_s'] * 1e3:.1f} ms, exponent {e:.2f}"})
    base = pts[0]["peak_rss"]
    e = exponent(a, b, "peak_rss", base)
    if e is not None and e > PREP_EXPONENT and b["peak_rss"] - base >= RSS_FLOOR:
        out.append({"kind": "prep_superlinear", "var": var,
                    "sizes": f"{a['size']} -> {b['size']}",
                    "detail": f"peak RSS over the smallest size "
                              f"{(a['peak_rss'] - base) / 2**20:.0f} -> "
                              f"{(b['peak_rss'] - base) / 2**20:.0f} MiB, "
                              f"exponent {e:.2f}"})
    return out


def per_element_jumps(var, points):
    """Adjacent timed sizes where ns per element rises by JUMP or more."""
    pts = [p for p in points if p.get("ns") and p["size"] > 0]
    out = []
    for a, b in zip(pts, pts[1:]):
        rise = (b["ns"] / b["size"]) / (a["ns"] / a["size"])
        if rise >= JUMP:
            out.append({"kind": "per_element_jump", "var": var,
                        "sizes": f"{a['size']} -> {b['size']}",
                        "detail": f"ns per element {a['ns'] / a['size']:.1f} -> "
                                  f"{b['ns'] / b['size']:.1f} "
                                  f"({m_ratio(rise)} per element)",
                        "pair": [a["size"], b["size"]]})
    return out


def m_ratio(r):
    return f"{r:.2f}x slower" if r >= 1 else f"{1 / r:.2f}x faster"


def shape(p):
    return (frozenset(p.get("declines") or ()),
            frozenset((p.get("lowering") or {}).keys()),
            frozenset((p.get("wa_lowering") or {}).keys()))


def program_changes_shape(var, points):
    pts = [p for p in points if p["status"] == "ok"]
    out = []
    for a, b in zip(pts, pts[1:]):
        sa, sb = shape(a), shape(b)
        if sa == sb:
            continue
        parts = []
        for label, x, y in (("decline", sa[0], sb[0]),
                            ("log_prob lowering", sa[1], sb[1]),
                            ("write_array lowering", sa[2], sb[2])):
            parts += [f"+{label} {v}" for v in sorted(y - x)]
            parts += [f"-{label} {v}" for v in sorted(x - y)]
        out.append({"kind": "program_changes_shape", "var": var,
                    "sizes": f"{a['size']} -> {b['size']}",
                    "detail": "; ".join(parts)})
    return out


def failures(var, points):
    ok = [p for p in points if p["status"] == "ok"]
    bad = [p for p in points if p["status"] == "failure"]
    if not ok or not bad:
        return []
    return [{"kind": "failure", "var": var, "sizes": str(p["size"]),
             "detail": p["message"]} for p in bad]


def detect(var, points):
    return (ops_grow(var, points) + prep_superlinear(var, points)
            + per_element_jumps(var, points)
            + program_changes_shape(var, points) + failures(var, points))


# ---------------------------------------------------------------------------
# Confirmation and CmdStan


def confirm(tools, x, f, lad, driver):
    """Time the finding's two sizes again, interleaved, and the same two
    sizes in CmdStan."""
    a, b = (next(p for p in lad["points"] if p["size"] == s) for s in f["pair"])
    comp = m.compile_source(tools, pathlib.Path(x["stan"]).read_text())
    st = m.paired_ratio(m.stanli_cmd(tools, comp, a["data"], None),
                        m.stanli_cmd(tools, comp, b["data"], None),
                        PAIRS, tools.timeout)
    f["stanli_ratio"] = st["ratio"]
    grow = b["size"] / a["size"]
    if f["kind"] == "per_element_jump":
        f["confirmed"] = st["ratio"] / grow >= JUMP
        if not f["confirmed"]:
            f["detail"] += (f"; not confirmed: interleaved pairs give "
                            f"{m_ratio(st['ratio'] / grow)} per element")
    if "exe" not in driver:
        f["cmdstan"] = f"CmdStan build failed: {driver.get('error', '')[:80]}"
        return
    cs = m.paired_ratio(m.cmdstan_cmd(driver, a["data"], None),
                        m.cmdstan_cmd(driver, b["data"], None), PAIRS,
                        tools.timeout)
    f["cmdstan_ratio"] = cs["ratio"]
    f["normalized"] = st["ratio"] / cs["ratio"]
    f["stanli_vs_cmdstan_at_larger"] = st["b_ns"] / cs["b_ns"]
    f["cmdstan"] = (f"stanli {m_ratio(st['ratio'])}, CmdStan "
                    f"{m_ratio(cs['ratio'])} from {a['size']} to {b['size']}; "
                    f"normalized {f['normalized']:.2f}; at {b['size']} stanli "
                    f"is {m_ratio(f['stanli_vs_cmdstan_at_larger'])} than CmdStan")


def run_scaling(args, tools):
    todo = sources(args.variants, args.only)
    meta = {"stage": "scaling", "load_1min": [m.load_average()]}

    def one(src):
        try:
            x = structural(tools, src, args)
        except Exception as e:  # keep the sweep going
            x = {"name": src[0], "error": f"harness: {e!r}", "ladders": {}}
        x["stan"] = str(src[1])
        print(f"structure {x['name']}: {len(x.get('ladders', {}))} ladders, "
              f"{len(x.get('refused', {}))} refused {x.get('error', '')}",
              flush=True)
        return x

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        models = list(pool.map(one, todo))
    if not args.no_timing:
        for x in models:
            time_ladders(tools, x, args, meta)
            print(f"timed {x['name']}", flush=True)
    for x in models:
        x["findings"] = [f for var, lad in x.get("ladders", {}).items()
                         for f in detect(var, lad["points"])]
    if not args.no_timing:
        wanted = [x for x in models if any(f.get("pair") for f in x["findings"])]
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            drivers = dict(zip([x["name"] for x in wanted], pool.map(
                lambda x: m.cmdstan_driver(
                    tools, pathlib.Path(x["stan"]).read_text()), wanted)))
        for x in wanted:
            started = time.monotonic()
            for f in x["findings"]:
                if f.get("pair"):
                    meta["load_1min"].append(m.load_average())
                    try:
                        confirm(tools, x, f, x["ladders"][f["var"]],
                                drivers[x["name"]])
                    except RuntimeError as e:
                        f["cmdstan"] = f"timing failed: {e}"
            x["cmdstan_s"] = time.monotonic() - started
    return models, meta
