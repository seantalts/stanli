import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools" / "perturb"))
import ladder  # noqa: E402
import run  # noqa: E402


def point(size, **kw):
    p = {"size": size, "status": "ok", "ops": 20, "wa_ops": 5, "prep_s": 0.01,
         "peak_rss": 10 << 20, "declines": [], "lowering": {}, "wa_lowering": {}}
    p.update(kw)
    return p


class Detectors(unittest.TestCase):
    def test_flat_ops_do_not_grow(self):
        pts = [point(n) for n in (8, 100, 1000, 10000)]
        self.assertEqual(ladder.ops_grow("N", pts), [])

    def test_linear_ops_grow(self):
        pts = [point(n, ops=3 * n) for n in (8, 100, 1000, 10000)]
        [f] = ladder.ops_grow("N", pts)
        self.assertEqual(f["graph"], "log_prob")
        self.assertAlmostEqual(f["slope"], 1.0)
        self.assertEqual(f["pair"], [8, 10000])

    def test_small_sizes_do_not_count_for_growth(self):
        pts = [point(1, ops=5), point(2, ops=9), point(3, ops=13),
               point(8, ops=20), point(100, ops=20), point(1000, ops=20)]
        self.assertEqual(ladder.ops_grow("N", pts), [])

    def test_prep_quadratic_is_superlinear(self):
        pts = [point(n, prep_s=1e-8 * n * n) for n in (100, 1000, 10000)]
        [f] = ladder.prep_superlinear("N", pts)
        self.assertIn("exponent 2.00", f["detail"])

    def test_prep_linear_is_fine(self):
        pts = [point(n, prep_s=1e-5 * n) for n in (100, 1000, 10000)]
        self.assertEqual(ladder.prep_superlinear("N", pts), [])

    def test_rss_growth_is_measured_over_the_smallest_size(self):
        base = 100 << 20
        pts = [point(n, peak_rss=base + n * n * 8) for n in (10, 1000, 10000)]
        kinds = [f["detail"] for f in ladder.prep_superlinear("N", pts)]
        self.assertTrue(any("peak RSS" in d for d in kinds))

    def test_per_element_jump_names_both_sizes(self):
        pts = [point(32, ns=320.0), point(33, ns=330.0), point(64, ns=640.0),
               point(65, ns=1300.0)]
        [f] = ladder.per_element_jumps("N", pts)
        self.assertEqual(f["sizes"], "64 -> 65")
        self.assertEqual(f["pair"], [64, 65])

    def test_shape_change_between_adjacent_sizes(self):
        pts = [point(32), point(33, declines=["reroll/log_prob: lane widths do not agree"],
                                lowering={"LOOP": 1})]
        [f] = ladder.program_changes_shape("N", pts)
        self.assertEqual(f["sizes"], "32 -> 33")
        self.assertIn("+decline reroll/log_prob", f["detail"])
        self.assertIn("+log_prob lowering LOOP", f["detail"])

    def test_failure_only_when_some_size_works(self):
        bad = {"size": 10**5, "status": "failure", "message": "bad_alloc"}
        self.assertEqual(len(ladder.failures("N", [point(8), bad])), 1)
        self.assertEqual(ladder.failures("N", [bad]), [])

    def test_rejected_sizes_are_not_failures(self):
        rej = {"size": 0, "status": "rejected", "message": "N must be >= 1"}
        self.assertEqual(ladder.failures("N", [point(8), rej]), [])


class Records:
    def __init__(self, compile):
        self.compile = compile


class ClassifyCheck(unittest.TestCase):
    def test_stan_math_errors_in_transformed_data_reject_the_size(self):
        rec = Records({"ok": False, "stage": "model", "error": "out_of_range"})
        self.assertEqual(ladder.classify_check(("compile_fail", "index 3"), rec)[0],
                         "rejected")
        rec = Records({"ok": False, "stage": "lower", "error": "out_of_range"})
        self.assertEqual(ladder.classify_check(("compile_fail", "index 3"), rec)[0],
                         "rejected")

    def test_index_errors_stanli_finds_while_lowering_reject_the_size(self):
        rec = Records({"ok": False, "stage": "model", "error": "compile_error"})
        msg = ("vector[uni] assign: accessing element out of range. index 2 out "
               "of range; expecting index to be between 1 and 1")
        self.assertEqual(ladder.classify_check(("refused", msg), rec)[0], "rejected")

    def test_stanli_refusing_to_lower_is_a_failure(self):
        rec = Records({"ok": False, "stage": "lower", "error": "compile_error"})
        self.assertEqual(ladder.classify_check(("compile_fail", "no rule"), rec)[0],
                         "failure")
        self.assertEqual(ladder.classify_check(("crash", "exit -11"), Records(None))[0],
                         "failure")
        self.assertEqual(ladder.classify_check(("refused", "no lane rule for OP_X"),
                                               rec)[0], "failure")


class CapSizes(unittest.TestCase):
    def test_points_bracket_the_program_budget(self):
        per = 448
        pts = [point(n, program_bytes=per * 10 * n, lowered_ops=10 * n)
               for n in (100, 1000)]
        cross = (8 << 20) / (per * 10)
        sizes = [s for s, why in ladder.cap_sizes(pts) if why.startswith("program")]
        self.assertEqual(len(sizes), 2)
        self.assertLess(sizes[0], cross)
        self.assertGreater(sizes[1], cross)

    def test_flat_program_has_no_cap_points(self):
        pts = [point(n, program_bytes=5000, lowered_ops=20) for n in (100, 1000)]
        self.assertEqual(ladder.cap_sizes(pts), [])

    def test_ladder_respects_the_lower_bound(self):
        class SV:
            lower = 1
        self.assertNotIn(0, ladder.ladder_sizes(SV, None))
        SV.lower = 0
        self.assertIn(0, ladder.ladder_sizes(SV, None))
        self.assertEqual(ladder.ladder_sizes(SV, [5, 3, 3]), [3, 5])


class Text(unittest.TestCase):
    def test_ratio_text(self):
        self.assertEqual(run.ratio_text(2.0), "2.00x slower")
        self.assertEqual(run.ratio_text(0.5), "2.00x faster")
        self.assertEqual(run.ratio_text(None), "-")

    def test_write_array_differences_do_not_invalidate(self):
        diffs = [("one_side_threw", {"point": 0, "write_array": True}),
                 ("output", {"point": 0})]
        self.assertIsNone(run.first_problem(diffs))
        self.assertIn("grad differs",
                      run.first_problem([("grad", {"point": 1, "none": "1", "o1": "2"})]))


if __name__ == "__main__":
    unittest.main()
