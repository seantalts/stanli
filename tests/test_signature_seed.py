"""Each signature replay writes its own context seed file.

Concurrent ctest replays used to share build/context_seed.json, and Windows
refuses to replace a file another process has open. A private file per run
leaves nothing to race on.
"""

import importlib.util
import pathlib
import shutil
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location(
    "check_signature_models", ROOT / "tools" / "check_signature_models.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PrivateSeed(unittest.TestCase):
    def setUp(self):
        self.build = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.build, ignore_errors=True)

    def replay(self):
        seen = []

        def replay_all(args):
            seen.append((args.data, args.data.read_text()))
            return 0

        with mock.patch.object(MODULE, "replay_all", replay_all, create=True), \
                mock.patch.object(sys, "argv",
                                  ["check", "--build", str(self.build)]):
            self.assertEqual(MODULE.main(), 0)
        return seen[0]

    def test_seed_is_written_before_the_replay_runs(self):
        _, text = self.replay()
        self.assertEqual(text, MODULE.json.dumps(MODULE.CONTEXT_DATA) + "\n")

    def test_seed_is_not_shared_through_the_build_directory(self):
        path, _ = self.replay()
        self.assertNotEqual(path.parent, self.build.resolve())
        self.assertEqual(list(self.build.iterdir()), [])

    def test_each_run_has_its_own_file_and_removes_it(self):
        first, _ = self.replay()
        second, _ = self.replay()
        self.assertNotEqual(first, second)
        self.assertFalse(first.exists())
        self.assertFalse(second.exists())


if __name__ == "__main__":
    unittest.main()
