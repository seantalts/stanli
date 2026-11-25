#!/usr/bin/env python3
"""Group structural findings by their cause.

    tools/perturb/causes.py SCALING_OUT DATAVALUES_OUT [--out causes.md]

Reads SCALING_OUT/scaling.json and DATAVALUES_OUT/datavalues.json, files
that run.py writes with --stage scaling and --stage datavalues. Each
finding is given the causes that explain it: a decline reason that is new
at the larger size or with the changed data, or a switch in how a loop is
lowered (LOOP, REGION_MAP, ISLAND). A finding with no such cause counts
under "no new decline". The table ranks the causes by how many models show
them, so it says which proof condition fails most often as sizes and data
values move.
"""
from __future__ import annotations

import argparse
import collections
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "tools"))
from corpus_inventory import corpus_cases  # noqa: E402

STRUCTURAL = ("ops_grow", "prep_superlinear", "program_changes_shape",
              "data_value_changes_program", "failure")
NONE = "no new decline or lowering switch"
# Below this size a lane group is too small to fuse at all, so a change of
# shape there says little about a proof condition.
MIN_SHAPE_SIZE = 8


def last_ok(points):
    good = [p for p in points if p.get("status") == "ok"]
    return good[-1] if good else {}


def scaling_causes(x, f):
    """The causes of one scaling finding."""
    kind = f["kind"]
    if kind == "program_changes_shape":
        return [c[1:].replace("decline ", "") for c in f["detail"].split("; ")
                if c.startswith("+")] or [NONE]
    if kind == "failure":
        return ["failure: " + re.sub(r"\d+", "N", f["detail"])[:90]]
    p = last_ok(x["ladders"][f["var"]]["points"])
    graph = f.get("graph", "log_prob")
    causes = [d for d in p.get("declines") or ()
              if kind != "ops_grow" or f"/{graph}:" in d]
    key = "wa_lowering" if graph == "write_array" else "lowering"
    causes += [f"{graph} lowering {op}" for op in p.get(key) or {}]
    return causes or [NONE]


def datavalue_causes(f):
    causes = list(f.get("new_declines") or ())
    causes += ["log_prob " + c[1:] for c in f["detail"].split("; ")
               if c.startswith("+lowering")]
    return causes or [NONE]


def where(x, f):
    if f["kind"] == "data_value_changes_program":
        t = f" (from {f['threshold']} duplicate rows)" if f.get("threshold") else ""
        return f"{x['name']} {f['var']} {f['sizes']}{t}"
    return f"{x['name']} {f['var']} {f.get('sizes', '')}"


def table(scaling, datavalues, collection_of):
    rows = collections.defaultdict(lambda: {"kinds": collections.Counter(),
                                             "models": set(), "examples": {}})
    for source, kind_of in ((scaling, "scaling"), (datavalues, "datavalues")):
        for x in source:
            for f in x.get("findings", []):
                if f["kind"] not in STRUCTURAL:
                    continue
                if (f["kind"] == "program_changes_shape"
                        and int(f["sizes"].split()[0]) < MIN_SHAPE_SIZE):
                    continue
                causes = (datavalue_causes(f) if kind_of == "datavalues"
                          else scaling_causes(x, f))
                model = x["name"].split("/")[0]
                for c in set(causes):
                    r = rows[c]
                    r["kinds"][f["kind"]] += 1
                    r["models"].add(model)
                    if model not in r["examples"] and len(r["examples"]) < 2:
                        r["examples"][model] = where(x, f)
    ranked = sorted(rows.items(), key=lambda kv: (-len(kv[1]["models"]), kv[0]))
    out = ["| cause | models | collections | findings by kind | examples |",
           "|---|---:|---:|---|---|"]
    for cause, r in ranked:
        cols = {collection_of.get(mm, "?") for mm in r["models"]}
        kinds = ", ".join(f"{k} {n}" for k, n in r["kinds"].most_common())
        out.append(f"| {cause} | {len(r['models'])} | {len(cols)} | {kinds} "
                   f"| {'; '.join(r['examples'].values())} |")
    return out


def refusals(scaling):
    reasons = collections.Counter()
    for x in scaling:
        for why in (x.get("refused") or {}).values():
            reasons[why.split(":")[0]] += 1
        if x.get("error"):
            reasons[x["error"].split(":")[0]] += 1
        elif not x.get("ladders") and not x.get("refused"):
            reasons["no int size variable in the data block"] += 1
    return reasons


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("scaling", type=pathlib.Path)
    ap.add_argument("datavalues", type=pathlib.Path)
    ap.add_argument("--out", type=pathlib.Path)
    args = ap.parse_args(argv)
    scaling = json.loads((args.scaling / "scaling.json").read_text())["models"]
    datavalues = json.loads(
        (args.datavalues / "datavalues.json").read_text())["models"]
    collection_of = {n: c.collection for n, c in corpus_cases(
        HERE.parents[1] / "deps" / "posteriordb", "all").items()}
    scaled = sum(1 for x in scaling if x.get("ladders"))
    lines = ["# Structural findings by cause", "",
             f"{len(scaling)} models swept; {scaled} have a size variable "
             f"that scale.py scales; {len(datavalues)} were given data-value "
             "perturbations.", ""]
    lines += table(scaling, datavalues, collection_of)
    lines += ["", "## Size variables scale.py refused, by reason", "",
              "| reason | variables |", "|---|---:|"]
    lines += [f"| {why} | {n} |" for why, n in refusals(scaling).most_common()]
    text = "\n".join(lines) + "\n"
    if args.out:
        args.out.write_text(text)
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
