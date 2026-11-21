#!/usr/bin/env python3
"""dump_ops prints the operations inside a retained loop, under the loop op."""
import pathlib
import re
import subprocess
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import bench_select as select

DUMP_OPS = None
FIXTURE = REPO / "tests/fixtures/structured_matrix_ops.tmir.sexp"
EMPTY_DATA = REPO / "tests/function_coverage/empty.json"
BODY_OPS = ("MATRIX_EXP", "QUAD_FORM_SYM", "MDIVIDE_LEFT", "MDIVIDE_RIGHT_SPD", "CROSSPROD")


def dump():
    return subprocess.check_output([DUMP_OPS, FIXTURE, EMPTY_DATA, "1000"], text=True)


class LoopBodyTests(unittest.TestCase):
    def setUp(self):
        self.text = dump()
        self.lines = self.text.splitlines()

    def test_the_header_and_top_level_ops_keep_their_format(self):
        self.assertRegex(self.lines[0], r"^slots=\d+ ops=3 result=\d+$")
        top = [l for l in self.lines if re.match(r"^ {0,4}\d+ \w+ +v=", l)]
        self.assertEqual([l.split()[1] for l in top[:3]], ["LOOP", "INDEX", "ADD"])

    def test_body_ops_follow_the_loop_op_in_the_same_line_format(self):
        loop = next(i for i, l in enumerate(self.lines) if re.match(r"^ *\d+ LOOP +v=", l))
        after = self.lines[loop + 1:]
        indent = len(self.lines[loop]) - len(self.lines[loop].lstrip())
        body = [l for l in after if re.match(r"^ *\d+ \w+ +v=", l)
                and len(l) - len(l.lstrip()) > indent]
        self.assertTrue(body, self.text)
        names = {l.split()[1] for l in body}
        for name in BODY_OPS:
            self.assertIn(name, names)

    def test_bench_select_sees_the_body_opcodes(self):
        counts = select.opcode_counts(self.text)
        for name in BODY_OPS:
            self.assertGreater(counts[name], 0, name)
        self.assertEqual(select.used_opcodes(self.text, ["OP_MATRIX_EXP"]), ["MATRIX_EXP"])

    def test_a_change_inside_the_body_is_a_program_change(self):
        changed = self.text.replace(" MATRIX_EXP ", " MATRIX_EXP_DYNAMIC ", 1)
        base = dict(mir="aa", ops=self.text, wa_ops="", islands="graph: 3 ops, 7 slots\n")
        new = dict(base, ops=changed)
        self.assertRegex(select.program_change(base, new), r"-MATRIX_EXP.*\+MATRIX_EXP_DYNAMIC")


if __name__ == "__main__":
    DUMP_OPS = sys.argv.pop(1)
    unittest.main()
