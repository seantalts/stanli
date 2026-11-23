"""The per-model tables in TESTING.md.

Every cell comes from what the corpus replay reads: the recorded CmdStan
references, tests/ulp_ledger.json and the limits in tools/verify_refs.py.
Nothing here runs a model, so tools/gen_docs.py can check the tables without
a build or a posteriordb checkout.
"""
import collections

import verify_refs as v
from corpus_inventory import COLLECTIONS, local_cases

LEDGER_PLATFORM = "Darwin arm64"
CLOSER_TEXT = {"stanli": "stanli closer", "cmdstan": "CmdStan closer",
               "unknown": "undecided"}
KINDS = ("lp", "grad", "wa")


def _sci(x):
    return f"{x:.1e}".replace("e-0", "e-").replace(".0e", "e")


def _count(n):
    return f"{n:,}" if n < 1e6 else f"{n:.1e}".replace("e+", "e")


def _cell(entries):
    """One table cell for the ledger entries of one output kind: the largest
    bound for each verdict, or the default."""
    if not entries:
        return str(v.DEFAULT_ULP_BOUND)
    worst = {}
    for entry in entries:
        worst[entry["closer"]] = max(worst.get(entry["closer"], 0), entry["bound"])
    return "; ".join(f"{_count(worst[c])} ({CLOSER_TEXT[c]})"
                     for c in ("cmdstan", "unknown", "stanli") if c in worst)


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


def corpus_rows():
    refs = v.load_refs()[0]
    ledger = v.load_ulp_ledger()["platforms"][LEDGER_PLATFORM]["main"]
    collection = _collections(refs)
    order = {name: i for i, name in enumerate(COLLECTIONS)}
    rows = []
    for model in sorted(refs, key=lambda m: (order[collection[m]], m.lower())):
        ref = refs[model]
        compared = [pt for pt in ref["points"].values() if "values" in pt]
        kinds = _by_kind(ledger.get(model, {}))
        if compared:
            with_wa = [pt for pt in compared if "wa" in pt]
            cells = [str(len(compared[0]["values"]) - 1),
                     str(len(with_wa[0]["wa"]["values"])) if with_wa else "0",
                     _cell(kinds["lp"]), _cell(kinds["grad"]),
                     _cell(kinds["wa"]) if with_wa else "not compared"]
        else:
            cells = ["", "", "", "", ""]
        rows.append({"model": model, "collection": collection[model],
                     "entries": ledger.get(model, {}), "cells": cells,
                     "note": _note(model, ref)})
    return rows


def render_corpus_table(rows):
    lines = ["| model | source | parameters | outputs | lp | gradient "
             "| write_array | also |",
             "| --- | --- | ---: | ---: | --- | --- | --- | --- |"]
    for row in rows:
        lines.append(f"| `{row['model']}` | {row['collection']} | "
                     + " | ".join(row["cells"]) + f" | {row['note']} |")
    return "\n".join(lines)


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
    return {
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
        "testing_models_no_wa": str(sum(row["cells"][4] == "not compared"
                                        for row in rows)),
        "testing_floored_models": str(len(set(v.ILL_CONDITIONED)
                                          | set(v.MISMATCH_OUTPUTS))),
        "testing_brms_limited": str(sum(m in refs for m in v.ULP_LIMITS)),
        "testing_brms_largest": str(sum(m in refs
                                        for m in v.GRADIENT_AGAINST_LARGEST)),
    }
