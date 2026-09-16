#!/usr/bin/env python3
"""Check fixture inventory, reference completeness, and optional regeneration.

python3 tools/check_rethinking.py [--generated DIR]
Does not run R or change an oracle. Numerical replay is verify_refs.py's job.
"""
import argparse
import csv
import pathlib

from verify_refs import POINTS, REPO, load_refs


def check(generated=None):
    root = REPO / "tests" / "rethinking"
    with (root / "inventory.tsv").open() as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    names = [row["file"] for row in rows]
    if len(set(names)) != len(names):
        raise ValueError("duplicate fixture in inventory")
    book = [(row["code_box"], row["model"]) for row in rows
            if row["code_box"] != "-"]
    if len(book) != 61 or len(set(book)) != 61:
        raise ValueError("expected all 61 distinct book call sites")
    expected = {f"{name}{ext}" for name in names for ext in (".stan", ".json")}
    actual = {p.name for p in root.iterdir() if p.suffix in (".stan", ".json")}
    if actual != expected:
        raise ValueError(f"fixture inventory differs: {sorted(actual ^ expected)}")
    readme = (root / "README.md").read_text()
    for row in rows:
        entry = (f"| [`{row['file']}.stan`]({row['file']}.stan) | "
                 f"`{row['model']}` | {row['code_box']} |")
        if entry not in readme:
            raise ValueError(f"{row['file']}: missing or stale README inventory row")
    refs, _ = load_refs()
    for name in names:
        if name not in refs or set(refs[name]["points"]) != set(map(str, POINTS)):
            raise ValueError(f"{name}: missing CmdStan reference points")
    if generated is not None:
        artifacts = expected | {"inventory.tsv"}
        produced = {p.name for p in generated.iterdir()
                    if p.suffix in (".stan", ".json", ".tsv")}
        if produced != artifacts:
            raise ValueError(f"regeneration file set differs: {sorted(produced ^ artifacts)}")
        changed = [name for name in sorted(artifacts)
                   if (root / name).read_bytes() != (generated / name).read_bytes()]
        if changed:
            raise ValueError(f"regeneration is not byte-identical: {' '.join(changed)}")
    print(f"{len(book)} book calls; {len(names)} fixtures; "
          f"{len(names) * len(POINTS)} reference points; inventory complete")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated", type=pathlib.Path)
    args = parser.parse_args()
    check(args.generated)
