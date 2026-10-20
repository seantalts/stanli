import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
import validate_total_estimates as v

CSV = """# stan_version_major = 2
lp__,accept_stat__,stepsize__,treedepth__,n_leapfrog__,divergent__,energy__,mu
-1,0.9,0.5,2,3,0,1,0.1
-1,0.9,0.5,1,1,0,1,0.2
# Adaptation terminated
# Step size = 0.4
-1,0.9,0.4,3,7,0,1,0.3
#
#  Elapsed Time: 0.25 seconds (Warm-up)
#                0.5 seconds (Sampling)
#                0.75 seconds (Total)
#
"""


class ParseTest(unittest.TestCase):
    def test_leapfrogs_and_elapsed_time(self):
        with tempfile.TemporaryDirectory() as d:
            path = pathlib.Path(d) / "out.csv"
            path.write_text(CSV)
            parsed = v.parse_cmdstan_csv(path)
        self.assertEqual(parsed["n_leapfrog"], [3, 1, 7])
        self.assertEqual(parsed["seconds"], {"warmup": 0.25, "sampling": 0.5})

    def test_incomplete_csv_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            path = pathlib.Path(d) / "out.csv"
            path.write_text(CSV.split("#  Elapsed")[0])
            with self.assertRaises(ValueError):
                v.parse_cmdstan_csv(path)


class RenderTest(unittest.TestCase):
    def test_ratio_row(self):
        row = dict(model="m", count=1000, ns_grad=dict(stanli=1000.0, cmdstan=2000.0),
                   estimated_s=dict(stanli=0.001, cmdstan=0.002), actual_s=dict(stanli=0.002, cmdstan=0.003),
                   leapfrog=dict(stanli=1000, cmdstan=1100, file=1000))
        text = v.render([row, dict(model="f", error="fallback model: x")])
        self.assertIn("| m | 1000 | 0.001 | 0.002 | 0.002 | 0.003 | 2 | 1.5 | 1000 | 1100 | 1.100 |", text)
        self.assertIn("fallback model: x", text)
        self.assertIn("0.75", text)


if __name__ == "__main__":
    unittest.main()
