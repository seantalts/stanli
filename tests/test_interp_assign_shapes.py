#!/usr/bin/env python3
"""stanli_check --interp throws CmdStan's message when a matrix assigned to
an array element has another shape.

  test_interp_assign_shapes.py STANLI_CHECK
"""
import os
import pathlib
import subprocess
import sys
import tempfile

FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures"

# CmdStan 2.40 with tools/ref_driver.cpp, at every point.
CMDSTAN = {
    "viewc_array_matrix_single_index_transpose_mismatch":
        "matrix assign columns: assigning variable a (3) and right hand side "
        "columns (2) must match in size",
    "viewc_array_matrix_prefix_mismatch":
        "matrix[omni] assign rows: assigning variable a (2) and right hand "
        "side rows (3) must match in size",
}


def main():
    check = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        data = pathlib.Path(tmp) / "empty.json"
        data.write_text("{}")
        for model, want in CMDSTAN.items():
            out = subprocess.run(
                [check, FIXTURES / f"{model}.stan", data, "--interp",
                 "--points", "0,3"], capture_output=True, text=True,
                env=dict(os.environ, STANLI_INTERP_NO_GRAD="1")).stdout
            got = [line for line in out.splitlines()
                   if not line.startswith(("POINT", "GRAD_ERR", "WANAMES",
                                           "WAVALS"))]
            if got != ["EVAL_FAIL " + want] * 2:
                print(f"FAIL {model}: {got}")
                failures += 1
    print("FAILED" if failures else "OK")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
