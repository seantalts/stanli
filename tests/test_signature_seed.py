"""The signature replay lands its shared context seed under concurrent ctest.

Windows refuses to replace a file another process has open, so the rename
that publishes the seed must retry, and must accept the file when another
process already landed the same text.
"""

import importlib.util
import os
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

TEXT = '{"seed": 1}\n'
REAL_REPLACE = os.replace


class LandSeed(unittest.TestCase):
    def setUp(self):
        self.dir = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.path = self.dir / "context_seed.json"
        sleep = mock.patch.object(MODULE.time, "sleep")
        sleep.start()
        self.addCleanup(sleep.stop)

    def staged(self):
        return [p.name for p in self.dir.iterdir() if p != self.path]

    def test_writes_a_missing_file(self):
        MODULE.land_seed(self.path, TEXT)
        self.assertEqual(self.path.read_text(), TEXT)
        self.assertEqual(self.staged(), [])

    def test_replaces_stale_text(self):
        self.path.write_text("old\n")
        MODULE.land_seed(self.path, TEXT)
        self.assertEqual(self.path.read_text(), TEXT)
        self.assertEqual(self.staged(), [])

    def test_leaves_matching_text_alone(self):
        self.path.write_text(TEXT)
        with mock.patch.object(MODULE.os, "replace") as replace:
            MODULE.land_seed(self.path, TEXT)
        replace.assert_not_called()
        self.assertEqual(self.staged(), [])

    def test_retries_a_denied_replace(self):
        denied = [PermissionError(13, "Access is denied")] * 3

        def replace(src, dst):
            if denied:
                raise denied.pop()
            REAL_REPLACE(src, dst)

        with mock.patch.object(MODULE.os, "replace", side_effect=replace):
            MODULE.land_seed(self.path, TEXT)
        self.assertEqual(self.path.read_text(), TEXT)
        self.assertEqual(self.staged(), [])

    def test_accepts_denial_when_another_process_landed_the_text(self):
        self.path.write_text("old\n")

        def replace(src, dst):
            self.path.write_text(TEXT)
            raise PermissionError(13, "Access is denied")

        with mock.patch.object(MODULE.os, "replace", side_effect=replace) as r:
            MODULE.land_seed(self.path, TEXT)
        self.assertEqual(r.call_count, 1)
        self.assertEqual(self.path.read_text(), TEXT)
        self.assertEqual(self.staged(), [])

    def test_raises_when_denied_for_good(self):
        self.path.write_text("old\n")
        denied = PermissionError(13, "Access is denied")
        with mock.patch.object(MODULE.os, "replace", side_effect=denied) as r:
            with self.assertRaises(PermissionError):
                MODULE.land_seed(self.path, TEXT, attempts=4)
        self.assertEqual(r.call_count, 4)
        self.assertEqual(self.path.read_text(), "old\n")
        self.assertEqual(self.staged(), [])


if __name__ == "__main__":
    unittest.main()
