import json
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools" / "perturb"))
import ladder  # noqa: E402
import measure  # noqa: E402
import report  # noqa: E402
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

    def test_prep_exponent_skips_close_sizes(self):
        pts = [point(100, prep_s=0.1), point(1000, prep_s=1.0),
               point(1030, prep_s=1.2)]
        self.assertEqual(ladder.prep_superlinear("N", pts), [])

    def test_write_array_growth_is_not_timed(self):
        pts = [point(n, wa_ops=3 * n) for n in (8, 100, 1000)]
        [f] = ladder.ops_grow("N", pts)
        self.assertEqual(f["graph"], "write_array")
        self.assertIsNone(f["pair"])

    def test_op_jumps_ignore_proportional_growth(self):
        linear = [point(n, ops=3 * n) for n in (100, 1000)]
        self.assertEqual(ladder.ops_jumps(linear), [])
        jump = [point(100, ops=27), point(1000, ops=1118)]
        self.assertEqual(ladder.ops_jumps(jump), [(100, 1000)])

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
        msg = "assignment width mismatch for b"
        self.assertEqual(ladder.classify_check(("refused", msg), rec)[0], "rejected")
        msg = "index offset: dimension exceeds the graph immediate range"
        self.assertEqual(ladder.classify_check(("refused", msg), rec)[0], "failure")

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
        self.assertEqual(ladder.ladder_sizes(SV, None), [0, 1, 8, 100, 10**4])
        self.assertIn(33, ladder.ladder_sizes(SV, None, full=True))


def outcome(values):
    """A differential.run_outcome payload with OK rows at the check points."""
    return ("points", {"points": {k: {"status": ("OK", [str(v) for v in values[k]])}
                                  for k in range(3)}, "prints": {}})


class Variants(unittest.TestCase):
    def test_work_not_dose_sets_the_expected_slowdown(self):
        rec = {"meta": {"equivalent": False, "dose": 40, "work": 4},
               "timing": {"ratio": 5.0}}
        self.assertFalse(run.needs_cmdstan(rec))
        rec["timing"]["ratio"] = 5.3
        self.assertTrue(run.needs_cmdstan(rec))

    def test_without_work_a_variant_always_goes_to_cmdstan(self):
        for dose in (0, 40, None):
            rec = {"meta": {"equivalent": False, "dose": dose},
                   "timing": {"ratio": 1.0}}
            self.assertTrue(run.needs_cmdstan(rec))

    def test_work_of_zero_is_a_work_value(self):
        self.assertEqual(run.expected_work({"work": 0}), 0.0)

    def test_largest_difference(self):
        a = outcome([[-10.0, 1.9e-3, 5.0]] * 3)
        b = outcome([[-10.0, 1.9e-3 + 1e-11, 5.0]] * 3)
        self.assertAlmostEqual(run.largest_difference(a, b, False), 1e-11 / 1.9e-3,
                               delta=1e-12)
        c = outcome([[-9.0, 1.9e-3, 5.0]] * 3)
        self.assertEqual(run.largest_difference(a, c, True), 0.0)

    def test_timing_cache_round_trip(self):
        import tempfile, types
        with tempfile.TemporaryDirectory() as d:
            tools = types.SimpleNamespace(cache=pathlib.Path(d))
            rec = {"outcome": "fine", "timing": {"ratio": 1.1}, "seed": "x"}
            run.save_timed(tools, "ab" * 32, rec, "done")
            got = run.load_timed(tools, "ab" * 32)
            self.assertEqual(got, {"phase": "done", "outcome": "fine",
                                   "timing": {"ratio": 1.1}})
            self.assertIsNone(run.load_timed(tools, "cd" * 32))

    def test_prep_cliff(self):
        rec = {"meta": {"equivalent": True},
               "base_program": {"prep_s": 0.048}, "variant_program": {"prep_s": 0.4}}
        self.assertTrue(run.prep_cliff(rec))
        rec["variant_program"]["prep_s"] = 0.12
        self.assertFalse(run.prep_cliff(rec))


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


def lanes(op, reason, graph="log_prob", pass_="reroll", period=1):
    return ("LANES " + json.dumps({"pass": pass_, "graph": graph, "op": op,
                                   "reason": reason, "period": period}))


STDERR = "\n".join([
    lanes(3, "group is too small"),
    lanes(9, "group is too small"),
    lanes(9, "group is too small", graph="bounded_log_prob"),
    lanes(4, "mixed ops", pass_="partition"),
    "stanli_region_map refused (n): assigns mu declared outside the body",
    "stanli_region_map refused (n): assigns mu declared outside the body",
    "stanli_region_map refused (i): assigns tmp declared outside the body",
    "stanli_region_map refused (k): no generated adjoint program_code=17 instructions=210",
    "stanli_region_map selected (j): iterations=200 prologue=11",
    "stanli_structured refused: a read of eta, which a branch on parameters may have left empty",
    "stanli_structured native candidate (n): something",
    "stanli_structured unrolled loop=n trips=200 graph=lp ops=40 delta=3",
    "slots=40 ops=16 result=39",
])


class Declines(unittest.TestCase):
    def test_every_pass_is_parsed_with_its_name(self):
        self.assertEqual(measure.lane_declines(STDERR), [
            "partition/log_prob: mixed ops",
            "region_map/log_prob: assigns NAME declared outside the body",
            "region_map/log_prob: no generated adjoint program_code=N instructions=N",
            "reroll/log_prob: group is too small",
            "structured_loop/log_prob: a read of NAME, which a branch on parameters "
            "may have left empty",
        ])

    def test_counts_are_one_per_lane_op_and_one_per_refused_loop(self):
        counts = measure.decline_counts(STDERR)
        self.assertEqual(counts["reroll/log_prob: group is too small"], 2)
        self.assertEqual(
            counts["region_map/log_prob: assigns NAME declared outside the body"], 2)
        self.assertEqual(counts["partition/log_prob: mixed ops"], 1)

    def test_the_same_loop_refused_by_two_lowering_tries_counts_once(self):
        text = "\n".join(["stanli_region_map refused (n): body has effects"] * 2)
        self.assertEqual(measure.decline_counts(text),
                         {"region_map/log_prob: body has effects": 1})

    def test_normalize_keeps_readable_text(self):
        self.assertEqual(measure.normalize_reason("body does more than add to lp"),
                         "body does more than add to NAME")
        self.assertEqual(measure.normalize_reason("fewer than two iterations"),
                         "fewer than two iterations")
        self.assertEqual(
            measure.normalize_reason("structured assignment changes runtime shape: mu"),
            "structured assignment changes runtime shape: NAME")

    def test_diagnostic_switches_are_set_for_the_structure_run(self):
        for name in ("STANLI_LANE_REPORT", "STANLI_REGION_MAP_DIAGNOSTICS",
                     "STANLI_STRUCTURED_LOOP_DIAGNOSTICS"):
            self.assertEqual(measure.DECLINE_ENV[name], "1")


class NewDeclines(unittest.TestCase):
    A = "reroll/log_prob: group is too small"
    B = "region_map/log_prob: body has effects"

    def test_a_reason_the_base_lacks_is_new(self):
        base = {"declines": [self.A], "decline_counts": {self.A: 2}}
        var = {"declines": [self.A, self.B],
               "decline_counts": {self.A: 2, self.B: 1}}
        self.assertEqual(measure.decline_growth(base, var), {self.B: 1})

    def test_a_reason_the_base_has_is_new_when_its_count_rises(self):
        base = {"declines": [self.A], "decline_counts": {self.A: 2}}
        var = {"declines": [self.A], "decline_counts": {self.A: 7}}
        self.assertEqual(measure.decline_growth(base, var), {self.A: 5})

    def test_a_falling_count_is_not_new(self):
        base = {"declines": [self.A], "decline_counts": {self.A: 7}}
        var = {"declines": [self.A], "decline_counts": {self.A: 2}}
        self.assertEqual(measure.decline_growth(base, var), {})

    def test_records_without_counts_compare_their_reason_sets(self):
        base = {"declines": [self.A]}
        var = {"declines": [self.A, self.B]}
        self.assertEqual(measure.decline_growth(base, var), {self.B: 1})

    def test_report_writes_the_rise_and_keeps_the_reason_for_grouping(self):
        rec = {"new_declines": [self.A, self.B],
               "decline_more": {self.A: 5}}
        self.assertEqual(report.decline_text(rec, self.A), self.A + " (x5 more)")
        self.assertEqual(report.decline_text(rec, self.B), self.B)
        self.assertEqual(report.decline_text({"new_declines": [self.A]}, self.A),
                         self.A)
        self.assertIn(self.A + " (x5 more)", report.note_of(
            {"outcome": "fine", **rec}))

    def test_a_variant_with_more_of_a_known_decline_is_not_unexplained(self):
        rec = {"new_declines": [self.A], "decline_more": {self.A: 5}}
        self.assertFalse(report.unexplained(rec))


if __name__ == "__main__":
    unittest.main()
