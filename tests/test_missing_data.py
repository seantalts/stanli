"""Data that leaves out a data-block variable is a clear error naming every
missing variable."""
import json
import os
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


ZERO_SIZE_MODEL = """
data { int N; array[N] real y; int K; vector[K] x; matrix[K, 2] M; }
parameters { real mu; }
model { y ~ normal(mu, 1); target += sum(x) + sum(M); }
"""

ZERO_SIZE_GQ_MODEL = """
data { int N; array[N] real y; int K; vector[K] x; }
parameters { real mu; }
model { y ~ normal(mu, 1); }
generated quantities { real s = sum(x); int n = num_elements(x); }
"""


def prepare(data, model=MODEL, env=None):
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        stan = directory / "model.stan"
        stan.write_text(model)
        mir = subprocess.run([STANC, "--O1", "--debug-optimized-mir", str(stan)],
                             check=True, capture_output=True, text=True).stdout
        (directory / "model.sexp").write_text(mir)
        (directory / "data.json").write_text(json.dumps(data))
        return subprocess.run([BENCH_GRAD, str(directory / "model.sexp"),
                               str(directory / "data.json"), "--prep"],
                              capture_output=True, text=True,
                              env={**os.environ, **(env or {})})


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

    def test_omitted_zero_size_variables_prepare(self):
        run = prepare({"N": 2, "y": [0.5, 1.5], "K": 0}, ZERO_SIZE_MODEL)
        self.assertEqual(run.returncode, 0, run.stderr)

    def test_omitted_zero_size_matches_explicit_empty(self):
        run = prepare({"N": 2, "y": [0.5, 1.5], "K": 0, "x": [], "M": []},
                      ZERO_SIZE_MODEL)
        self.assertEqual(run.returncode, 0, run.stderr)

    def test_omitted_zero_size_variable_reaches_write_array(self):
        for mode in ("0", "force"):
            with self.subTest(STANLI_PARALLEL_WA=mode):
                run = prepare({"N": 2, "y": [0.5, 1.5], "K": 0},
                              ZERO_SIZE_GQ_MODEL, {"STANLI_PARALLEL_WA": mode})
                self.assertEqual(run.returncode, 0, run.stderr)

    def test_omitted_scalar_is_still_missing(self):
        run = prepare({"N": 2, "y": [0.5, 1.5]}, ZERO_SIZE_MODEL)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("missing data for K, x, M", run.stderr)

    def test_omitted_nonempty_array_is_still_missing(self):
        run = prepare({"N": 2, "K": 0}, ZERO_SIZE_MODEL)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("missing data for y;", run.stderr)

    def test_omitted_array_of_nonzero_declared_size_is_missing(self):
        run = prepare({"N": 2, "y": [0.5, 1.5], "K": 1}, ZERO_SIZE_MODEL)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("missing data for x, M", run.stderr)


if __name__ == "__main__":
    BENCH_GRAD, STANC = sys.argv.pop(1), sys.argv.pop(1)
    unittest.main()
