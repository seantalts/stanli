"""Matrix callbacks preserve engines, solver outputs, and direct-RK parity."""
from collections import Counter
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from verify_refs import parse_status, parse_wa


def callbacks(node):
    if isinstance(node, dict):
        if "callback" in node:
            yield node
        for value in node.values():
            yield from callbacks(value)
    elif isinstance(node, list):
        for value in node:
            yield from callbacks(value)


def main(check, dump_ops):
    for suffix in ("", "_fallback"):
        stem = ROOT / "tests/fixtures" / ("matrix_callback_contexts" + suffix)
        report = json.loads(subprocess.check_output(
            [dump_ops, str(stem.with_suffix(".tmir.sexp")),
             str(stem.with_suffix(".json")), "--execution-json"], text=True))
        selected = list(callbacks(report["log_prob"]))
        assert Counter(node["operation"] for node in selected) == {
            "OP_ODE": 2, "OP_DAE": 2, "OP_ALGEBRA_SOLVER": 2,
            "OP_QUADRATURE": 2, "OP_ODE_ADJOINT": 2}, selected
        for node in selected:
            callback = node["callback"]
            assert callback["value_engine"] == (
                "mir_interpreter" if suffix else "register_program"), callback
        # This existing caller limitation deliberately exercises value-only
        # interpretation and its separate retained-callback argument packer.
        assert report["write_array"]["value_engine"] == "mir_interpreter"
        assert "data argument must be data-only" in report["write_array"]["refusal"]

    for name in ("ode_matrix_callback", "ode_matrix_callback_flat"):
        stem = ROOT / "tests/fixtures" / name
        report = json.loads(subprocess.check_output(
            [dump_ops, str(stem.with_suffix(".tmir.sexp")),
             str(stem.with_suffix(".json")), "--execution-json"], text=True))
        selected = list(callbacks(report["log_prob"]))
        assert selected and all(n["callback"]["value_engine"] == "register_program" for n in selected)
        assert all(n["direct_rk_enabled"] for n in selected)
        for point in range(3):
            results = []
            for disabled in (False, True):
                env = os.environ.copy()
                env.pop("STANLI_NO_ODE_DIRECT_RK", None)
                if disabled:
                    env["STANLI_NO_ODE_DIRECT_RK"] = "1"
                run = subprocess.run([check, str(stem.with_suffix(".stan")),
                    str(stem.with_suffix(".json")), "--mir",
                    str(stem.with_suffix(".tmir.sexp")), "--wa-values", "--point", str(point)],
                    env=env, capture_output=True, text=True, check=True)
                values = parse_status(run.stdout)
                assert values[0] == "OK", run.stdout
                wa = parse_wa(run.stdout)
                assert wa
                results.append((wa[0], [struct.pack("d", float(v)) for v in [*values[1:], *wa[1]]]))
            assert results[0] == results[1], (name, point, results)
    print("Matrix callback engine selection and direct-RK bitwise parity OK")


if __name__ == "__main__":
    main(*sys.argv[1:])
