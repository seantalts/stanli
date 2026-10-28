"""Data that leaves out a data-block variable is a clear error naming every
missing variable."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

MODEL = """
data { int I; array[I] int y; int J; vector[J] x; }
parameters { real<lower=0, upper=1> p; }
model { y ~ bernoulli(p); }
"""


def prepare(data):
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        stan = directory / "model.stan"
        stan.write_text(MODEL)
        mir = subprocess.run([STANC, "--O1", "--debug-optimized-mir", str(stan)],
                             check=True, capture_output=True, text=True).stdout
        (directory / "model.sexp").write_text(mir)
        (directory / "data.json").write_text(json.dumps(data))
        return subprocess.run([BENCH_GRAD, str(directory / "model.sexp"),
                               str(directory / "data.json"), "--prep"],
                              capture_output=True, text=True)


class MissingData(unittest.TestCase):
    def test_every_missing_variable_is_named(self):
        run = prepare({"J": 1})
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("missing data for I, y, x", run.stderr)

    def test_partial_data_names_the_rest(self):
        run = prepare({"I": 2, "J": 1, "x": [0.5]})
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("missing data for y", run.stderr)

    def test_complete_data_prepares(self):
        run = prepare({"I": 2, "y": [0, 1], "J": 1, "x": [0.5]})
        self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    BENCH_GRAD, STANC = sys.argv.pop(1), sys.argv.pop(1)
    unittest.main()
