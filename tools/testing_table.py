"""The per-model tables and the counts in TESTING.md.

Every cell comes from what the corpus replay reads: the recorded CmdStan
references, tests/ulp_ledger.json and the limits in tools/verify_refs.py.
Nothing here runs a model, so tools/gen_docs.py can check the tables without
a build or a posteriordb checkout.
"""
import collections
import json

import verify_refs as v
from corpus_inventory import COLLECTIONS, local_cases

LEDGER_PLATFORM = "Darwin arm64"
CROSS_PATH_LEDGER = v.REPO / "tests" / "cross_path_ledger.json"
ACCURACY = v.REPO / "docs" / "internal" / "artifacts" / "accuracy.json"
CLOSER_TEXT = {"stanli": "stanli closer", "cmdstan": "CmdStan closer",
               "unknown": "undecided"}
KINDS = ("lp", "grad", "wa")


def _sci(x):
    return f"{x:.1e}".replace("e-0", "e-").replace(".0e", "e")


def _count(n):
    return f"{n:,}" if n < 1e6 else f"{n:.1e}".replace("e+", "e")


def _cell(entries):
    """One table cell for one ledger entry or more: the largest bound for
    each verdict, or the default."""
    if not entries:
        return str(v.DEFAULT_ULP_BOUND)
    worst = {}
    for entry in entries:
        worst[entry["closer"]] = max(worst.get(entry["closer"], 0), entry["bound"])
    return "; ".join(f"{_count(worst[c])} ({CLOSER_TEXT[c]})"
                     for c in ("cmdstan", "unknown", "stanli") if c in worst)


KIND_NAME = {"lp": "lp", "grad": "gradient", "wa": "outputs"}


def _limit(kinds):
    """The limit against CmdStan for one model: the default, or the largest
    ledger bound of each output kind that has one."""
    over = [f"{KIND_NAME[kind]} {_count(max(e['bound'] for e in kinds[kind]))}"
            for kind in KINDS if kinds[kind]]
    return ", ".join(over) or str(v.DEFAULT_ULP_BOUND)


def _mean(x):
    """A mean distance in ULP, to two significant digits."""
    if x == 0:
        return "0"
    if x < 0.005:
        return "<0.01"
    if x < 10:
        return f"{x:.2f}"
    if x < 1e5:
        return f"{x:,.0f}" if x >= 100 else f"{x:.1f}"
    return f"{x:.1e}".replace("e+0", "e").replace("e+", "e")


def _measured(record):
    """(CmdStan cell, stanli cell) for one output kind of one model."""
    if not record or "cmdstan" not in record:
        return ("", "")
    return (_mean(record["cmdstan"]["mean"]), _mean(record["stanli"]["mean"]))


def _by_kind(entries):
    kinds = collections.defaultdict(list)
    for label, entry in entries.items():
        kinds[v.output_kind(label)].append(entry)
    return kinds


def _brms_limit(model):
    limit = v.ULP_LIMITS.get(model)
    if limit is None:
        return None
    if model in v.GRADIENT_AGAINST_LARGEST:
        return f"brms limit {limit}, gradient against its largest entry"
    return f"brms limit {limit}"


def _note(model, ref):
    notes = []
    points = ref["points"]
    rejected = sorted(int(p) for p, pt in points.items() if "values" not in pt)
    if len(rejected) == len(points):
        notes.append("CmdStan rejects every point, and stanli must too")
    elif rejected:
        notes.append("CmdStan rejects point "
                     + " and ".join(map(str, rejected))
                     + ", and stanli must too")
    if model in v.ILL_CONDITIONED:
        lp_gate = v.ILL_CONDITIONED_LP_GATE.get(model)
        notes.append("ill-conditioned: the 1e-9 limit is "
                     f"{_sci(v.ILL_CONDITIONED_GATE)} for the gradient"
                     + (f" and {_sci(lp_gate)} for lp" if lp_gate else ""))
    if model in v.MISMATCH_OUTPUTS:
        indices = v.MISMATCH_OUTPUTS[model].indices
        notes.append("known mismatch: "
                     + ", ".join(f"grad[{i}]" for i in indices)
                     + " have their own relative limit")
    limit = _brms_limit(model)
    if limit and len(rejected) < len(points):
        notes.append(limit)
    return "; ".join(notes)


def _collections(refs):
    local = local_cases(include_language=True)
    return {model: local[model].collection if model in local else "posteriordb"
            for model in refs}


def load_accuracy():
    return json.loads(ACCURACY.read_text())


def corpus_rows():
    refs = v.load_refs()[0]
    ledger = v.load_ulp_ledger()["platforms"][LEDGER_PLATFORM]["main"]
    accuracy = load_accuracy()["models"]
    collection = _collections(refs)
    order = {name: i for i, name in enumerate(COLLECTIONS)}
    rows = []
    for model in sorted(refs, key=lambda m: (order[collection[m]], m.lower())):
        ref = refs[model]
        compared = [pt for pt in ref["points"].values() if "values" in pt]
        kinds = _by_kind(ledger.get(model, {}))
        measured = accuracy.get(model, {})
        if compared:
            with_wa = [pt for pt in compared if "wa" in pt]
            cells = [str(len(compared[0]["values"]) - 1),
                     str(len(with_wa[0]["wa"]["values"])) if with_wa else "0"]
            for kind in KINDS:
                cells += _measured(measured.get(kind))
            cells.append(_limit(kinds))
        else:
            cells = [""] * 9
        rows.append({"model": model, "collection": collection[model],
                     "entries": ledger.get(model, {}), "cells": cells,
                     "has_wa": bool(compared and with_wa),
                     "measured": measured, "note": _note(model, ref)})
    return rows


def render_corpus_table(rows):
    lines = ["| model | source | parameters | outputs | lp: CmdStan | lp: stanli "
             "| gradient: CmdStan | gradient: stanli | outputs: CmdStan "
             "| outputs: stanli | limit | also |",
             "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: "
             "| ---: | --- | --- |"]
    for row in rows:
        lines.append(f"| `{row['model']}` | {row['collection']} | "
                     + " | ".join(row["cells"]) + f" | {row['note']} |")
    return "\n".join(lines)


def accuracy_stats(rows):
    """Counts over the measured distances from the 80-digit values."""
    out = {}
    worse_models, worse_values = set(), 0
    closer = collections.Counter()
    values = 0
    for kind in KINDS:
        scored = [(row["model"], row["measured"][kind]) for row in rows
                  if "cmdstan" in row["measured"].get(kind, {})]
        better = sum(r["stanli"]["mean"] < r["cmdstan"]["mean"] for _, r in scored)
        worse = sum(r["stanli"]["mean"] > r["cmdstan"]["mean"] for _, r in scored)
        out[f"testing_scored_{kind}"] = str(len(scored))
        out[f"testing_scored_{kind}_stanli"] = str(better)
        out[f"testing_scored_{kind}_cmdstan"] = str(worse)
        out[f"testing_scored_{kind}_same"] = str(len(scored) - better - worse)
        for model, record in scored:
            values += record["n"]
            closer["stanli"] += record["stanli_closer"]
            closer["cmdstan"] += record["cmdstan_closer"]
            if record.get("worse_count"):
                worse_models.add(model)
                worse_values += record["worse_count"]
    out["testing_scored_values"] = f"{values:,}"
    out["testing_values_stanli_closer"] = f"{closer['stanli']:,}"
    out["testing_values_cmdstan_closer"] = f"{closer['cmdstan']:,}"
    out["testing_worse_models"] = str(len(worse_models))
    out["testing_worse_values"] = f"{worse_values:,}"
    return out


def render_collections(rows):
    count = collections.Counter(row["collection"] for row in rows)
    over = collections.Counter(row["collection"] for row in rows
                               if row["entries"])
    lines = ["| source | models | models with a value over "
             f"{v.DEFAULT_ULP_BOUND} ULP |",
             "| --- | ---: | ---: |"]
    for name in COLLECTIONS:
        if count[name]:
            lines.append(f"| {name} | {count[name]} | {over[name]} |")
    lines.append(f"| all | {len(rows)} | {sum(over.values())} |")
    return "\n".join(lines)


def render_local_ledger():
    """The local-corpus values with a ledger entry."""
    ledger = v.load_ulp_ledger()["platforms"][LEDGER_PLATFORM]
    lines = ["| corpus | model | value | limit | reason |",
             "| --- | --- | --- | --- | --- |"]
    for corpus in sorted(c for c in ledger if c != "main"):
        for model, entries in sorted(ledger[corpus].items()):
            for label, entry in sorted(entries.items()):
                reason = entry["reason"].split("; ", 1)[-1]
                lines.append(f"| `{corpus}` | `{model}` | `{label}` | "
                             f"{_cell([entry])} | {reason} |")
    return "\n".join(lines)


def stats():
    rows = corpus_rows()
    refs = v.load_refs()[0]
    entries = [entry for row in rows for entry in row["entries"].values()]
    closer = collections.Counter(entry["closer"] for entry in entries)
    points = [pt for ref in refs.values() for pt in ref["points"].values()]
    values = sum(len(pt.get("values", ())) + len(pt.get("wa", {}).get("values", ()))
                 for pt in points)
    recorded = load_accuracy()["recorded"]
    cross = json.loads(CROSS_PATH_LEDGER.read_text())
    unbounded = [entry for entry in cross if entry["bound_ulp"] == -1]
    return {
        "testing_cross_entries": str(len(cross)),
        "testing_cross_unbounded": str(len(unbounded)),
        "testing_cross_unbounded_everywhere": str(sum(
            "platform" not in entry for entry in unbounded)),
        "testing_corpus_table": render_corpus_table(rows),
        "testing_collections": render_collections(rows),
        "testing_local_ledger": render_local_ledger(),
        "testing_models": str(len(rows)),
        "testing_points": str(len(points)),
        "testing_rejected_points": str(sum("values" not in pt for pt in points)),
        "testing_values": f"{values:,}",
        "testing_models_within": str(sum(not row["entries"] for row in rows)),
        "testing_models_over": str(sum(bool(row["entries"]) for row in rows)),
        "testing_entries": str(len(entries)),
        "testing_entries_stanli": str(closer["stanli"]),
        "testing_entries_cmdstan": str(closer["cmdstan"]),
        "testing_entries_unknown": str(closer["unknown"]),
        "testing_models_no_wa": str(sum(bool(row["cells"][0])
                                        and not row["has_wa"] for row in rows)),
        **accuracy_stats(rows),
        "testing_accuracy_date": recorded["date"],
        "testing_accuracy_source": f"`{recorded['stanli_source'][:8]}`",
        "testing_floored_models": str(len(set(v.ILL_CONDITIONED)
                                          | set(v.MISMATCH_OUTPUTS))),
        "testing_brms_limited": str(sum(m in refs for m in v.ULP_LIMITS)),
        "testing_brms_largest": str(sum(m in refs
                                        for m in v.GRADIENT_AGAINST_LARGEST)),
    }
