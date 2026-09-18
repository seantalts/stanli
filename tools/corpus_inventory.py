#!/usr/bin/env python3
"""One inventory of model/data pairs for numerical tests and benchmarks.

Collection names describe provenance, not separate validation protocols.
Posteriordb entries retain the first dataset in sorted posterior-file order.
Language fixtures are included explicitly by numerical callers.
"""
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import zipfile

REPO = Path(__file__).resolve().parents[1]
COLLECTIONS = ("all", "posteriordb", "educational", "rethinking", "brms",
               "teaching", "stanc3")


@dataclass(frozen=True)
class CorpusCase:
    name: str
    source: Path
    data: Path
    collection: str
    metadata: dict


def source_digest(path):
    """Hash text independently of Git's Windows newline conversion."""
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def _insert(cases, case):
    if case.name in cases:
        raise ValueError(f"Duplicate corpus model id: {case.name}")
    cases[case.name] = case


def _imported_cases(root):
    manifest = json.loads((root / "manifest.json").read_text())
    entries = manifest["models"]
    names = [entry["model_id"] for entry in entries]
    actual = {p.name for p in (root / "models").iterdir() if p.is_dir()}
    if len(set(names)) != len(names) or set(names) != actual:
        raise ValueError("Imported corpus manifest does not match model directories")
    for entry in entries:
        # Attribution and the distinction between imported and synthetic data
        # travel with each case rather than being reconstructed by consumers.
        for key in ("source_url", "license", "license_file", "data_kind"):
            if not entry.get(key):
                raise ValueError(f"Missing imported provenance: {entry['model_id']} {key}")
        if not (root / entry["license_file"]).is_file():
            raise ValueError(f"Missing imported license: {entry['license_file']}")
        paths = []
        for key, sha in (("model_path", "sha256"), ("data_path", "data_sha256")):
            path = root / entry[key]
            if not path.resolve().is_relative_to(root.resolve()):
                raise ValueError(f"Imported fixture escapes collection: {entry[key]}")
            if source_digest(path) != entry[sha]:
                raise ValueError(f"Imported fixture changed: {entry[key]}")
            paths.append(path)
        yield CorpusCase(entry["model_id"], *paths, "educational",
                         dict(entry))


def local_cases(include_language=True):
    """Local fixtures keyed by unique model id, with validated provenance."""
    cases = {}
    collections = ["brms", "rethinking"]
    if include_language:
        collections.append("stanc3")
    for collection in collections:
        for source in sorted((REPO / "tests" / collection).glob("*.stan")):
            data = source.with_suffix(".json")
            if not data.is_file():
                raise ValueError(f"Missing corpus data: {data}")
            _insert(cases, CorpusCase(source.stem, source, data, collection, {}))
    for case in _imported_cases(REPO / "tests" / "educational"):
        _insert(cases, case)
    return dict(sorted(cases.items()))


def corpus_cases(pdb, collection="all", include_language=False):
    """Resolve the selected corpus, accepting a checkout or database path.

    PDB data remains zipped; use materialize_data for tools needing JSON.
    Explicit stanc3 selection enables language fixtures too.
    """
    if collection not in COLLECTIONS:
        raise ValueError(f"Unknown corpus collection: {collection}")
    pdb = Path(pdb)
    if (pdb / "posterior_database").is_dir():
        pdb = pdb / "posterior_database"
    cases = {}
    for path in sorted((pdb / "posteriors").glob("*.json")):
        meta = json.loads(path.read_text())
        name = meta["model_name"]
        if name in cases:
            continue
        _insert(cases, CorpusCase(name, pdb / "models" / "stan" / f"{name}.stan",
                                 pdb / "data" / "data" / f"{meta['data_name']}.json.zip",
                                 "posteriordb", meta))
    for case in local_cases(include_language or collection == "stanc3").values():
        _insert(cases, case)
    wanted = {"educational", "rethinking", "brms"} if collection == "teaching" else {collection}
    return {name: case for name, case in sorted(cases.items())
            if collection == "all" or case.collection in wanted}


def materialize_data(case, directory):
    """Return a JSON path, extracting PDB data to a model-specific file."""
    if case.data.suffix != ".zip" or not case.data.exists():
        return case.data
    target = Path(directory) / f"{case.name}_data.json"
    with zipfile.ZipFile(case.data) as archive:
        target.write_bytes(archive.read(archive.namelist()[0]))
    return target
