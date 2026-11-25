"""The data-value stage: same source, same sizes, different values.

Each perturbation changes the values of a model's data and keeps every size,
so its prepared program can be compared with the base data's. A program
that grows, or that lowers its loops another way, is a
`data_value_changes_program` finding. Duplicate rows are swept as a dose,
and the dose is bisected to the number of duplicate rows that changes the
program.
"""
from __future__ import annotations

import concurrent.futures
import copy
import json
import math
import random
import time

import ladder
import measure as m
import scale

DUPLICATE_FRACTIONS = (0.01, 0.1, 0.5)
LEVELS = (2, 5, 20)
MAX_COLUMNS = 3
OPS_RATIO = 1.5
OPS_FLOOR = 20
BISECT_STEPS = 10


# ---------------------------------------------------------------------------
# Which data a perturbation may touch


def row_axes(an):
    """{rows variable: [container names]} for every rows variable whose
    containers all have it as their first axis, so a row is one element."""
    out = {}
    for var, sv in an.vars.items():
        if sv.refused or sv.kind != "rows" or not sv.value:
            continue
        uses = an.containers(var)
        if uses and all(i == 0 for _, i in uses) and all(
                d.name in an.data and not an.zero_sized(d, an.data)
                for d, _ in uses):
            out[var] = [d.name for d, _ in uses]
    return out


def real_columns(an, names):
    """(container, column or None) for each real-valued column of the
    containers, with its values; constant columns are left out."""
    out = []
    for name in names:
        d, v = an.decls[name], an.data[name]
        if d.basetype not in scale.REAL_TYPES:
            continue
        if v and all(isinstance(r, list) and r and not isinstance(r[0], list)
                     for r in v):
            for j in range(len(v[0])):
                col = [r[j] for r in v]
                if len(set(col)) > 1:
                    out.append((name, j, col))
        elif v and not isinstance(v[0], list) and len(set(v)) > 1:
            out.append((name, None, list(v)))
    return out


def set_column(data, name, j, values):
    if j is None:
        data[name] = list(values)
    else:
        data[name] = [r[:j] + [x] + r[j + 1:] for r, x in zip(data[name], values)]


def permute_rows(data, names, order):
    for name in names:
        data[name] = [data[name][i] for i in order]


# ---------------------------------------------------------------------------
# The perturbations: (kind, label, dose, data)


def duplicate_rows(an, var, names, count, rng_seed=0):
    """count rows replaced by copies of other rows, chosen with a seed."""
    n = an.vars[var].value
    rng = random.Random(f"dup/{var}/{rng_seed}")
    targets = set(rng.sample(range(n), min(count, n - 1)))
    sources = [i for i in range(n) if i not in targets]
    data = copy.deepcopy(an.data)
    for t in sorted(targets):
        s = rng.choice(sources)
        for name in names:
            data[name][t] = copy.deepcopy(an.data[name][s])
    return data


def perturbations(an):
    out = []
    for var, names in row_axes(an).items():
        n = an.vars[var].value
        if n >= 4:
            for f in DUPLICATE_FRACTIONS:
                count = max(1, round(f * n))
                out.append(("duplicate_rows", f"{var} {f:g}", count,
                            duplicate_rows(an, var, names, count)))
            half = math.ceil(n / 2)
            tiled = copy.deepcopy(an.data)
            permute_rows(tiled, names, [i % half for i in range(n)])
            out.append(("duplicate_rows", f"{var} tiled", n - half, tiled))
        cols = real_columns(an, names)
        for levels in LEVELS:
            data = copy.deepcopy(an.data)
            for name, j, col in cols:
                set_column(data, name, j, quantize(col, levels))
            if cols:
                out.append(("discrete_covariate", f"{var} {levels} levels",
                            levels, data))
        for name, j, col in cols[:MAX_COLUMNS]:
            label = name if j is None else f"{name}[, {j + 1}]"
            data = copy.deepcopy(an.data)
            set_column(data, name, j, [col[0]] * len(col))
            out.append(("constant_column", label, None, data))
            d = an.decls[name]
            if bound_allows(d, 0.0):
                data = copy.deepcopy(an.data)
                set_column(data, name, j, [0.0] * len(col))
                out.append(("zero_column", label, None, data))
        out += intercept_columns(an, names)
        for g, up in an.index.items():
            if an.decls[g].axes[0] == var and an.vars.get(up):
                out += index_orders(an, var, names, g, up)
    return out


def quantize(col, levels):
    """Each value moved to the nearest of `levels` quantiles of the column."""
    xs = sorted(col)
    picks = sorted({xs[round(q * (len(xs) - 1) / max(1, levels - 1))]
                    for q in range(levels)})
    return [min(picks, key=lambda p: abs(p - x)) for x in col]


def bound_allows(d, x):
    lo, hi = scale._float_bound(d.lower), scale._float_bound(d.upper)
    return (lo is None or lo <= x) and (hi is None or x <= hi)


def intercept_columns(an, names):
    """An all-ones column added to each matrix whose columns are a size
    variable that scale.py can grow."""
    out = []
    for name in names:
        d = an.decls[name]
        if d.basetype != "matrix" or len(d.axes) != 2:
            continue
        k = d.axes[1]
        sv = an.vars.get(k)
        if sv is None or sv.refused or sv.value is None:
            continue
        try:
            data, _ = scale.scale(an.text, an.data, k, sv.value + 1, mode="tile")
        except (scale.Refused, ValueError):
            continue
        data[name] = [r[:-1] + [1.0] for r in data[name]]
        out.append(("intercept_column", f"{name} + ones", None, data))
    return out


def index_orders(an, var, names, g, up):
    n, groups = an.vars[var].value, an.vars[up].value
    rng = random.Random(f"order/{g}")
    rows = list(range(n))
    shuffled = rows[:]
    rng.shuffle(shuffled)
    out = []
    for label, order in (("sorted", sorted(rows, key=lambda i: an.data[g][i])),
                         ("reversed", rows[::-1]), ("shuffled", shuffled)):
        data = copy.deepcopy(an.data)
        permute_rows(data, names, order)
        out.append(("index_order", f"{g} {label}", None, data))
    data = copy.deepcopy(an.data)
    data[g] = [1] * n
    out.append(("index_order", f"{g} constant", None, data))
    data = copy.deepcopy(an.data)
    for i in rng.sample(rows, n // 2):
        data[g][i] = 1
    out.append(("index_order", f"{g} half in group 1", None, data))
    if groups and n >= groups > 1:
        data = copy.deepcopy(an.data)
        data[g] = [i + 2 if i < groups - 1 else 1 for i in range(n)]
        out.append(("unbalanced_groups", f"{g} {groups - 1} singletons", None,
                    data))
    return out


# ---------------------------------------------------------------------------
# Measuring and judging


def changed(base, p):
    """How the perturbed program differs from the base data's, or None."""
    if p.get("status") != "ok":
        return None
    parts = []
    bo, po = base.get("ops") or 0, p.get("ops") or 0
    if po >= OPS_RATIO * bo and po - bo >= OPS_FLOOR:
        parts.append(f"log_prob ops {bo} -> {po}")
    bl, pl = set(base.get("lowering") or {}), set(p.get("lowering") or {})
    parts += [f"+lowering {x}" for x in sorted(pl - bl)]
    parts += [f"-lowering {x}" for x in sorted(bl - pl)]
    if not parts:
        return None
    new = sorted(set(p.get("declines") or ()) - set(base.get("declines") or ()))
    return "; ".join(parts), new


def measure_data(tools, comp, data, path):
    path.write_text(json.dumps(data))
    try:
        return ladder.measure_point(tools, comp, path, 0)
    finally:
        path.unlink()


def bisect_duplicates(tools, comp, an, var, names, base, lo, hi, work):
    """The smallest duplicate count between lo (same program) and hi
    (changed) that changes the program."""
    for _ in range(BISECT_STEPS):
        if hi - lo <= 1:
            break
        mid = (lo + hi) // 2
        p = measure_data(tools, comp, duplicate_rows(an, var, names, mid),
                         work / "bisect.json")
        if changed(base, p):
            hi = mid
        else:
            lo = mid
    return hi


def one_model(tools, source, args):
    name, stan, data_path, _ = source
    started = time.monotonic()
    x = {"name": name, "perturbations": [], "findings": []}
    text = stan.read_text()
    data = json.loads(data_path.read_text())
    comp = m.compile_source(tools, text)
    if not comp["ok"]:
        x["error"] = "does not compile"
        return x
    try:
        an = scale.Analysis(text, data)
        todo = perturbations(an)
    except (scale.Refused, ValueError) as e:
        x["error"] = f"scale.py: {e}"
        return x
    work = tools.cache.parent / "datavalues" / name.replace("/", "__")
    work.mkdir(parents=True, exist_ok=True)
    base = ladder.measure_point(tools, comp, data_path, 0)
    x["base"] = {k: base.get(k) for k in ("status", "ops", "lowering",
                                           "declines", "message")}
    if base["status"] != "ok":
        x["error"] = f"base data: {base['status']} {base.get('message')}"
        return x
    deadline = started + args.model_seconds
    dup_seen = {}
    for kind, label, dose, new in todo:
        if time.monotonic() > deadline:
            x["stop"] = "the model's time budget is spent"
            break
        p = measure_data(tools, comp, new, work / "point.json")
        rec = {"kind": kind, "label": label, "dose": dose,
               **{k: p.get(k) for k in ("status", "message", "ops",
                                        "lowering", "declines", "prep_s",
                                        "executor_bytes")}}
        x["perturbations"].append(rec)
        diff = changed(base, p)
        if kind == "duplicate_rows":
            var = label.split()[0]
            if diff and var not in dup_seen and not label.endswith("tiled"):
                dup_seen[var] = True
                lower = max([r["dose"] for r in x["perturbations"]
                             if r["kind"] == kind and r["label"].startswith(var + " ")
                             and not changed(base, r) and r["status"] == "ok"
                             and not r["label"].endswith("tiled")], default=0)
                rec["threshold"] = bisect_duplicates(
                    tools, comp, an, var, row_axes(an)[var], base, lower, dose,
                    work)
        if diff:
            what, new_declines = diff
            x["findings"].append({
                "kind": "data_value_changes_program", "var": kind,
                "sizes": label, "detail": what, "new_declines": new_declines,
                "threshold": rec.get("threshold")})
    x["seconds"] = time.monotonic() - started
    return x


def run_datavalues(args, tools):
    todo = ladder.sources(args.variants, args.only)
    todo = [s for s in todo if s[3] is None]  # bases only
    meta = {"stage": "datavalues", "load_1min": [m.load_average()]}

    def one(src):
        try:
            x = one_model(tools, src, args)
        except Exception as e:  # keep the sweep going
            x = {"name": src[0], "error": f"harness: {e!r}", "findings": [],
                 "perturbations": []}
        print(f"datavalues {x['name']}: {len(x['perturbations'])} perturbed, "
              f"{len(x['findings'])} changed {x.get('error', '')}", flush=True)
        return x

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        models = list(pool.map(one, todo))
    return models, meta


def report(models, meta):
    out = ["# Data-value report", "",
           f"{len(models)} models, {meta['seconds']:.0f} s.", ""]
    rows = [(x, f) for x in models for f in x["findings"]]
    out += [f"{len(rows)} perturbations changed the program.", "",
            "| model | perturbation | value | change | new declines | threshold |",
            "|---|---|---|---|---|---|"]
    for x, f in rows:
        out.append(f"| {x['name']} | {f['var']} | {f['sizes']} | {f['detail']} "
                   f"| {'; '.join(f['new_declines'])[:200]} "
                   f"| {f.get('threshold') or '-'} |")
    errors = [x for x in models if x.get("error")]
    if errors:
        out += ["", "Not perturbed:", ""]
        out += [f"- {x['name']}: {x['error'][:160]}" for x in errors]
    return "\n".join(out) + "\n"
