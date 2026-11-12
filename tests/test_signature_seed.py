"""Each signature replay writes its own context seed file.

Concurrent ctest replays used to share build/context_seed.json, and Windows
refuses to replace a file another process has open. A private file per run
leaves nothing to race on.
"""

import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "harnesses"))
import verify_refs  # noqa: E402
from corpus_inventory import source_digest  # noqa: E402
from function_signature_common import CONTEXT_DATA  # noqa: E402


class PrivateSeed(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        source = self.tmp / "builtin_signatures_01.stan"
        source.write_text("parameters { real x; }\n")
        manifest = self.tmp / "manifest.json"
        manifest.write_text(json.dumps({"models": [
            {"file": str(source), "source_sha256": source_digest(source)}]}))
        self.refs = {"builtin_signatures_01": {
            "recorded": {}, "data_sha256": "", "points": {}}}
        self.manifests = (manifest,)

    def replay(self):
        seen = []

        def check_model(model, ref, pdb, check_bin, tmp, *args, opts):
            seen.append((opts.data, opts.data.read_text()))
            return (model, "OK", 0.0, 0, 0, "", [])

        with mock.patch.object(verify_refs, "SIGNATURE_MANIFESTS", self.manifests), \
                mock.patch.object(verify_refs, "load_corpus_refs",
                                  lambda corpus: (self.refs, {})), \
                mock.patch.object(verify_refs, "pin_drift", lambda *a: ""), \
                mock.patch.object(verify_refs, "check_model", check_model):
            self.assertEqual(verify_refs.replay_corpus(
                "signatures", [], self.tmp / "stanli_check", None, 1, 10), 0)
        return seen[0]

    def test_seed_is_written_before_the_replay_runs(self):
        _, text = self.replay()
        self.assertEqual(text, json.dumps(CONTEXT_DATA) + "\n")

    def test_seed_is_not_shared_through_the_repository(self):
        path, _ = self.replay()
        self.assertFalse(path.resolve().is_relative_to(ROOT))

    def test_each_run_has_its_own_file_and_removes_it(self):
        first, _ = self.replay()
        second, _ = self.replay()
        self.assertNotEqual(first, second)
        self.assertFalse(first.exists())
        self.assertFalse(second.exists())


if __name__ == "__main__":
    unittest.main()
