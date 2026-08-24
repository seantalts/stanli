#!/usr/bin/env python3
"""Populate web/models/ from a posteriordb checkout: the Stan source, the
unzipped data, and an index the demo page searches.

Only models that tools/verify_sample.py has verified against CmdStan get
in, so every model the page offers is one whose log density and full
gradient are known to match. Data files are shared: the eleven radon
models all read radon_all.json, so it is written once and fetched once.

The notes are hand written and live in tools/model_notes.json, keyed by
model name. Everything else on the card is derived: parameter count and
per-gradient speedup come from docs/corpus-bench.tsv, keywords from
posteriordb, so no number here can drift from the measured artifacts.

Usage: tools/gen_web_models.py [PDB_DIR] [--out DIR] [--check]

--check writes nothing and asks only whether every eligible model has a
note and no note is orphaned, which is the part CI can enforce without a
browser.
"""
import csv
import json
import pathlib
import sys
import zipfile

REPO = pathlib.Path(__file__).resolve().parent.parent
NOTES = REPO / "tools" / "model_notes.json"
VERIFY_JSON = REPO / "docs" / "verification.json"
BENCH_TSV = REPO / "docs" / "corpus-bench.tsv"

# nn_rbm1bJ100 reads all of MNIST: 179 MB of JSON for one model, against
# 7.8 MB for the other 117 put together. Nothing else comes close, so the
# cap is a backstop rather than a knob.
MAX_DATA_BYTES = 2.5 * 1024 * 1024

# Draw counts scale to the model, from the reference single-chain time for
# 1000 warmup + 1000 draws in docs/corpus-bench.tsv. A demo that looks hung
# is a worse demo than a short chain, and the browser is slower than the
# machine that produced these numbers.
DRAWS = [(5, 1000), (20, 500), (60, 250)]
DRAWS_MIN = 100


def draws_for(sample_s):
    if sample_s is None:
        return DRAWS_MIN
    for limit, n in DRAWS:
        if sample_s <= limit:
            return n
    return DRAWS_MIN


def num(row, key):
    try:
        return float(row[key])
    except (KeyError, TypeError, ValueError):
        return None


def eligible(pdb):
    """Every verified model with a dataset small enough to serve, as
    {model: (posterior meta, data bytes)}, deduplicated by model the way
    tools/corpus.py does it."""
    ver = json.loads(VERIFY_JSON.read_text())
    seen, out = set(), {}
    for pj in sorted((pdb / "posteriors").glob("*.json")):
        meta = json.loads(pj.read_text())
        model = meta["model_name"]
        if model in seen:
            continue
        seen.add(model)
        if ver.get(model, {}).get("status") != "VERIFIED":
            continue
        stan = pdb / "models" / "stan" / f"{model}.stan"
        dz = pdb / "data" / "data" / f"{meta['data_name']}.json.zip"
        if not stan.exists() or not dz.exists():
            continue
        with zipfile.ZipFile(dz) as z:
            name = z.namelist()[0]
            if z.getinfo(name).file_size > MAX_DATA_BYTES:
                continue
            out[model] = (meta, z.read(name))
    return out


def main():
    argv = sys.argv[1:]
    check = "--check" in argv
    argv = [a for a in argv if a != "--check"]
    out_dir = REPO / "web" / "models"
    if "--out" in argv:
        i = argv.index("--out")
        out_dir = pathlib.Path(argv[i + 1])
        del argv[i:i + 2]
    pdb_root = pathlib.Path(argv[0]) if argv else REPO / "deps" / "posteriordb"
    pdb = pdb_root / "posterior_database"
    if not pdb.is_dir():
        sys.exit(f"no posterior_database under {pdb_root}")

    notes = json.loads(NOTES.read_text())
    models = eligible(pdb)

    missing = sorted(m for m in models if not notes.get(m))
    orphan = sorted(n for n in notes if n not in models)
    if missing:
        print(f"no note for {len(missing)} models: "
              f"{', '.join(missing)}", file=sys.stderr)
    if orphan:
        print(f"note for {len(orphan)} models that are not eligible: "
              f"{', '.join(orphan)}", file=sys.stderr)
    if check:
        if missing or orphan:
            sys.exit(1)
        print(f"{len(models)} models, every one with a note")
        return
    if missing:
        sys.exit(1)

    bench = {}
    if BENCH_TSV.exists():
        with BENCH_TSV.open() as f:
            bench = {r["model"]: r for r in csv.DictReader(f, delimiter="\t")}

    data_dir = out_dir / "data"
    data_dir.mkdir(parents=True, exist_ok=True)
    for stale in list(out_dir.glob("*.stan")) + list(data_dir.glob("*.json")):
        stale.unlink()

    index, written = [], set()
    for model, (meta, raw) in sorted(models.items()):
        (out_dir / f"{model}.stan").write_bytes(
            (pdb / "models" / "stan" / f"{model}.stan").read_bytes())
        dn = meta["data_name"]
        if dn not in written:
            (data_dir / f"{dn}.json").write_bytes(raw)
            written.add(dn)
        row = bench.get(model, {})
        sample_s = num(row, "stanli_sample_s")
        us = num(row, "stanli_ns_grad")
        cm = num(row, "cmdstan_ns_grad")
        entry = {
            "name": model,
            "data": dn,
            "note": notes[model],
            "keywords": sorted({
                k.lower() for k in (meta.get("keywords") or [])
                if isinstance(k, str)}),
            "warmup": draws_for(sample_s),
            "samples": draws_for(sample_s),
        }
        if row.get("params"):
            entry["params"] = int(row["params"])
        if us:
            entry["us"] = round(us / 1000, 2)
        if us and cm:
            entry["speedup"] = round(cm / us, 2)
        index.append(entry)

    (out_dir / "index.json").write_text(
        json.dumps({"models": index}, indent=1, sort_keys=True) + "\n")
    total = sum(p.stat().st_size for p in out_dir.rglob("*"))
    print(f"{len(index)} models, {len(written)} datasets, "
          f"{total / 1024 / 1024:.2f} MB in {out_dir}")


if __name__ == "__main__":
    main()
