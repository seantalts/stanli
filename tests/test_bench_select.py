#!/usr/bin/env python3
"""Selection of the corpus models that a compiler-pipeline change must time."""
import csv
import pathlib
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import bench_select as select

OPS = """slots=9 ops=3 result=8
    0 CONCAT2    v=00 out=s3(len4) in=s1(l2),s2(l2)
    1 CONCAT2    v=00 out=s4(len6) in=s3(l4),s2(l2)
    2 TRANSPOSE  v=00 out=s5(len6) in=s4(l6)
"""
NEW_OPS = """slots=9 ops=3 result=8
    0 REP_MAT    v=00 out=s3(len6) in=s1(l1)
    1 SET_SLICE_STRIDED_INPLACE v=00 out=s3(len6) in=s3(l6),s2(l2)
    2 SET_SLICE_STRIDED_INPLACE v=00 out=s3(len6) in=s3(l6),s1(l2)
"""

LOOP_OPS = """slots=7 ops=2 result=6
    0 LOOP       v=00 out=s3(len2) in=s0(l1,P),s2(l1)
          body slots=9 ops=3 outputs=s3
              seq
                  while cond=s6
                      seq
                        0 COMPARE    v=02 out=s6(len1) in=s1(l1),s5(l1) storage=transient active=0
                       10 MATRIX_EXP v=00 out=s21(len4) in=s20(l4) idata=[2] storage=retained active=1
                          alias dst=s19 src=s21
    1 INDEX      v=00 out=s4(len1) in=s3(l2) idata=[0]
"""


def program(ops=OPS, islands="graph: 3 ops, 9 slots\n", mir="aa", wa_ops=OPS):
    return dict(mir=mir, ops=ops, wa_ops=wa_ops, islands=islands)


class OpcodeTests(unittest.TestCase):
    def test_counts_the_opcode_column_and_skips_the_header(self):
        self.assertEqual(select.opcode_counts(OPS), {"CONCAT2": 2, "TRANSPOSE": 1})

    def test_model_prints_before_the_dump_are_ignored(self):
        printed = "Number of groups: 1\n\nGroup 1: [1,2]\n" + OPS
        self.assertEqual(select.opcode_counts(printed), {"CONCAT2": 2, "TRANSPOSE": 1})

    def test_named_opcodes_match_whole_names_with_or_without_the_prefix(self):
        ops = "slots=1 ops=1 result=0\n    0 CATEGORICAL_LOGIT v=00 out=s0(len1) in=\n"
        self.assertEqual(select.used_opcodes(ops, ["OP_CATEGORICAL"]), [])
        self.assertEqual(select.used_opcodes(ops, ["CATEGORICAL_LOGIT"]), ["CATEGORICAL_LOGIT"])

    def test_opcodes_called_from_an_island_count_as_used(self):
        islands = "  FORWARD:\n    3  CALL      OP_LGAMMA r4 <- r2\n"
        self.assertEqual(select.used_opcodes("slots=1 ops=0 result=0\n", ["OP_LGAMMA"],
                                             islands), ["LGAMMA"])

    def test_opcodes_inside_a_retained_loop_body_count(self):
        self.assertEqual(select.opcode_counts(LOOP_OPS),
                         {"LOOP": 1, "COMPARE": 1, "MATRIX_EXP": 1, "INDEX": 1})
        self.assertEqual(select.used_opcodes(LOOP_OPS, ["OP_MATRIX_EXP", "OP_ADD"]),
                         ["MATRIX_EXP"])

    def test_control_lines_of_a_loop_body_are_not_opcodes(self):
        self.assertNotIn("while", select.opcode_counts(LOOP_OPS))
        self.assertNotIn("alias", select.opcode_counts(LOOP_OPS))

    def test_opcode_names_come_from_the_op_table(self):
        table = "  X(OP_ADD_N)                         \\\n  X(OP_CATEGORICAL)   \\\n"
        self.assertEqual(select.table_opcodes(table), {"ADD_N", "CATEGORICAL"})

    def test_list_entries_with_arguments_are_opcodes_too(self):
        table = ("  X(OP_NORMAL_LPDF, normal_lpdf, 3, 3)         \\\n"
                 "  X(OP_LOGIT)  \\\n")
        self.assertEqual(select.table_opcodes(table), {"NORMAL_LPDF", "LOGIT"})


class ChangeTests(unittest.TestCase):
    def test_identical_programs_are_unchanged(self):
        self.assertIsNone(select.program_change(program(), program()))

    def test_summary_names_the_opcode_count_changes(self):
        self.assertEqual(select.program_change(program(), program(ops=NEW_OPS)),
                         "ops 3 -> 3: -CONCAT2 x2, -TRANSPOSE, +REP_MAT, "
                         "+SET_SLICE_STRIDED_INPLACE x2")

    def test_a_change_only_inside_a_loop_body_is_a_program_change(self):
        replaced = LOOP_OPS.replace("MATRIX_EXP", "QUAD_FORM_SYM")
        self.assertEqual(select.program_change(program(LOOP_OPS), program(replaced)),
                         "ops 4 -> 4: -MATRIX_EXP, +QUAD_FORM_SYM")
        moved = LOOP_OPS.replace("storage=retained", "storage=transient")
        self.assertEqual(select.program_change(program(LOOP_OPS), program(moved)),
                         "ops 4 -> 4: same opcodes, different operands")

    def test_a_change_only_in_the_write_array_graph_is_a_program_change(self):
        replaced = LOOP_OPS.replace("MATRIX_EXP", "QUAD_FORM_SYM")
        self.assertEqual(select.program_change(program(wa_ops=LOOP_OPS),
                                               program(wa_ops=replaced)),
                         "write_array ops 4 -> 4: -MATRIX_EXP, +QUAD_FORM_SYM")
        self.assertEqual(select.program_change(program(wa_ops=""), program()),
                         "write_array ops 0 -> 3: +CONCAT2 x2, +TRANSPOSE")
        both = select.program_change(program(), program(ops=NEW_OPS, wa_ops=NEW_OPS))
        self.assertTrue(both.startswith("ops 3 -> 3: "), both)
        self.assertIn("; write_array ops 3 -> 3: ", both)

    def test_opcodes_used_only_by_the_write_array_count(self):
        self.assertEqual(select.program_opcodes(program(wa_ops=LOOP_OPS),
                                                ["OP_MATRIX_EXP", "OP_ADD"]),
                         ["MATRIX_EXP"])

    def test_a_model_without_a_write_array_graph_dumps_nothing(self):
        self.assertEqual(select.write_array_ops(1, "", "model has no write_array graph\n"),
                         ("", None))
        self.assertEqual(select.write_array_ops(0, OPS, ""), (OPS, None))
        self.assertEqual(select.write_array_ops(1, "", "boom\n"), (None, "exit 1"))

    def test_operand_and_island_and_mir_differences_are_reported(self):
        moved = OPS.replace("s1(l2),s2", "s2(l2),s1")
        self.assertEqual(select.program_change(program(), program(ops=moved)),
                         "ops 3 -> 3: same opcodes, different operands")
        self.assertEqual(select.program_change(program(), program(islands="graph: 4\n")),
                         "islands differ")
        self.assertEqual(select.program_change(program(), program(mir="bb")),
                         "MIR differs; programs identical")


class PreparationTests(unittest.TestCase):
    def test_regression_needs_two_percent_and_five_milliseconds(self):
        self.assertFalse(select.prep_regressed(0.010, 0.014))
        self.assertTrue(select.prep_regressed(0.010, 0.0151))
        self.assertFalse(select.prep_regressed(1.0, 1.019))
        self.assertTrue(select.prep_regressed(1.0, 1.021))


SUMMARY = REPO / "output/corpus-performance/benchmark-summary.tsv"
MAX_SPREAD = 0.015


def relative_spread(summary):
    with open(summary, newline="") as stream:
        return {row["model"]: float(row["paired_speedup_mad"]) / float(row["paired_speedup"])
                for row in csv.DictReader(stream, delimiter="\t") if row["paired_speedup"]}


class ControlTests(unittest.TestCase):
    def test_the_control_set_is_pinned(self):
        self.assertEqual(select.CONTROLS, (
            "i319_pois_re", "sw_ar", "eight_schools_noncentered", "hmm_example",
            "one_comp_mm_elim_abs", "gpcm_latent_reg_irt", "cm_lba2",
            "mlumr_survival_mspline_relaxed", "hierarchical_gp", "kronecker_gp",
            "oncobayes2_blrm_exnex_combo3"))

    def test_every_control_is_quiet_in_the_published_run(self):
        if not SUMMARY.exists():
            self.skipTest("no published corpus run")
        spread = relative_spread(SUMMARY)
        for name in select.CONTROLS:
            self.assertIn(name, spread)
            self.assertLessEqual(spread[name], MAX_SPREAD, name)


class SelectionTests(unittest.TestCase):
    def test_every_reason_selects_and_failures_are_never_dropped(self):
        chosen = select.selection(
            models=["a", "b", "c", "d", "e", "f"],
            changes={"a": "ops 3 -> 2: -TRANSPOSE"},
            opcode_users={"a": ["CATEGORICAL"], "b": ["CATEGORICAL"]},
            prep_flags={"c": "preparation 10.0 -> 20.0 ms"},
            failures={"d": "base dump_ops failed (exit -6)"},
            controls=["e"])
        self.assertEqual(chosen, {
            "a": ["program: ops 3 -> 2: -TRANSPOSE", "uses CATEGORICAL"],
            "b": ["uses CATEGORICAL"],
            "c": ["preparation 10.0 -> 20.0 ms"],
            "d": ["unresolved: base dump_ops failed (exit -6)"],
            "e": ["control"]})

    def test_unknown_controls_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "zz"):
            select.selection(models=["a"], changes={}, opcode_users={}, prep_flags={},
                             failures={}, controls=["zz"])


if __name__ == "__main__":
    unittest.main()
