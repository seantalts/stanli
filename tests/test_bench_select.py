#!/usr/bin/env python3
"""Selection of the corpus models that a compiler-pipeline change must time."""
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


def program(ops=OPS, islands="graph: 3 ops, 9 slots\n", mir="aa"):
    return dict(mir=mir, ops=ops, islands=islands)


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

    def test_opcode_names_come_from_the_op_table(self):
        table = "  X(OP_ADD_N)                         \\\n  X(OP_CATEGORICAL)   \\\n"
        self.assertEqual(select.table_opcodes(table), {"ADD_N", "CATEGORICAL"})


class ChangeTests(unittest.TestCase):
    def test_identical_programs_are_unchanged(self):
        self.assertIsNone(select.program_change(program(), program()))

    def test_summary_names_the_opcode_count_changes(self):
        self.assertEqual(select.program_change(program(), program(ops=NEW_OPS)),
                         "ops 3 -> 3: -CONCAT2 x2, -TRANSPOSE, +REP_MAT, "
                         "+SET_SLICE_STRIDED_INPLACE x2")

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
