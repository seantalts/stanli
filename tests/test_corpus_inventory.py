#!/usr/bin/env python3
"""Shared corpus identity, selection, provenance, and data resolution."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
import corpus_inventory as inventory


class InventoryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.imported = self.root / "tests/educational"
        (self.imported / "models").mkdir(parents=True)
        (self.imported / "LICENSE").write_text("fixture license")
        for collection in ("brms", "rethinking", "stanc3"):
            directory = self.root / "tests" / collection
            directory.mkdir()
            (directory / f"{collection}.stan").write_text("parameters { real x; }")
            (directory / f"{collection}.json").write_text("{}")
        self.manifest = {"models": []}
        for name in ("import_a", "import_b"):
            directory = self.imported / "models" / name
            directory.mkdir()
            (directory / "model.stan").write_text("parameters { real x; }\n")
            (directory / "data.json").write_text("{}\n")
            self.manifest["models"].append({"model_id": name,
                "model_path": f"models/{name}/model.stan",
                "data_path": f"models/{name}/data.json",
                "sha256": inventory.source_digest(directory / "model.stan"),
                "data_sha256": inventory.source_digest(directory / "data.json"),
                "source_url": "https://example.test/source", "license": "BSD-3-Clause",
                "license_file": "LICENSE", "data_kind": "synthetic_smoke",
                "sampling_smoke": name == "import_a"})
        self.write_manifest()
        self.pdb = self.root / "posterior_database"
        (self.pdb / "posteriors").mkdir(parents=True)
        (self.pdb / "models/stan").mkdir(parents=True)
        (self.pdb / "models/stan/pdb_model.stan").write_text("parameters { real x; }")
        (self.pdb / "data/data").mkdir(parents=True)
        for order, data in (("01", "first"), ("02", "second")):
            (self.pdb / "posteriors" / f"{order}.json").write_text(json.dumps(
                {"model_name": "pdb_model", "data_name": data}))
            with zipfile.ZipFile(self.pdb / "data/data" / f"{data}.json.zip", "w") as z:
                z.writestr("data.json", json.dumps({"dataset": data}))
        patcher = patch.object(inventory, "REPO", self.root)
        patcher.start()
        self.addCleanup(patcher.stop)

    def write_manifest(self):
        (self.imported / "manifest.json").write_text(json.dumps(self.manifest))

    def test_first_pdb_dataset_and_materialization_are_preserved(self):
        cases = inventory.corpus_cases(self.root)
        self.assertEqual(cases["pdb_model"].metadata["data_name"], "first")
        data = inventory.materialize_data(cases["pdb_model"], self.root)
        self.assertEqual(json.loads(data.read_text()), {"dataset": "first"})

    def test_filters_are_provenance_only_and_language_is_explicit(self):
        self.assertEqual(set(inventory.corpus_cases(self.pdb)),
                         {"pdb_model", "import_a", "import_b", "brms", "rethinking"})
        self.assertIn("stanc3", inventory.corpus_cases(self.pdb, include_language=True))
        self.assertEqual(set(inventory.corpus_cases(self.pdb, "stanc3")), {"stanc3"})
        self.assertEqual(set(inventory.corpus_cases(self.pdb, "teaching")),
                         {"import_a", "import_b", "brms", "rethinking"})

    def test_import_size_is_manifest_driven_and_metadata_survives(self):
        cases = inventory.corpus_cases(self.pdb, "educational")
        self.assertEqual(len(cases), 2)
        self.assertEqual(cases["import_a"].metadata, self.manifest["models"][0])
        self.assertFalse(cases["import_b"].metadata["sampling_smoke"])

    def test_duplicate_id_between_collections_is_rejected(self):
        (self.root / "tests/brms/pdb_model.stan").write_text("")
        (self.root / "tests/brms/pdb_model.json").write_text("{}")
        with self.assertRaisesRegex(ValueError, "Duplicate corpus model id"):
            inventory.corpus_cases(self.pdb)

    def test_duplicate_manifest_id_is_rejected(self):
        self.manifest["models"].append(self.manifest["models"][0])
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "manifest"):
            inventory.local_cases()

    def test_unmanifested_import_is_rejected(self):
        (self.imported / "models/unlisted").mkdir()
        with self.assertRaisesRegex(ValueError, "manifest"):
            inventory.local_cases()

    def test_source_and_data_edits_fail_hash_checks(self):
        for filename in ("model.stan", "data.json"):
            path = self.imported / "models/import_a" / filename
            original = path.read_bytes()
            path.write_bytes(original + b"changed")
            with self.assertRaisesRegex(ValueError, "fixture changed"):
                inventory.local_cases()
            path.write_bytes(original)

    def test_windows_newlines_do_not_change_fixture_identity(self):
        path = self.imported / "models/import_a/model.stan"
        text = path.read_bytes().replace(b"\r\n", b"\n")
        path.write_bytes(text.replace(b"\n", b"\r\n"))
        self.assertIn("import_a", inventory.local_cases())

    def test_missing_provenance_fails(self):
        del self.manifest["models"][0]["source_url"]
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "provenance"):
            inventory.local_cases()


if __name__ == "__main__":
    unittest.main()
