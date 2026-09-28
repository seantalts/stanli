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
        # Runtime data-only reals stay in the output graph. A refused callback
        # remains local to the solver instead of rejecting the whole block.
        wa = report["write_array"]
        assert wa.get("value_engine") != "mir_interpreter", wa
        wa_callbacks = list(callbacks(wa))
        assert len(wa_callbacks) == 5, wa
        for node in wa_callbacks:
            assert node["callback"]["value_engine"] == (
                "mir_interpreter" if suffix else "register_program"), node
            flags = node["variant"]
            assert flags == {"OP_ODE": 0x10, "OP_DAE": 0x8,
                             "OP_ALGEBRA_SOLVER": 0, "OP_QUADRATURE": 0,
                             "OP_ODE_ADJOINT": 0x10}[node["operation"]], node

    for name in ("ode_runtime_for", "for_bound_semantics", "ode_integer_arithmetic"):
        stem = ROOT / "tests/fixtures" / name
        report = json.loads(subprocess.check_output(
            [dump_ops, str(stem.with_suffix(".tmir.sexp")),
             str(stem.with_suffix(".json")), "--execution-json"], text=True))
        for phase in ("log_prob", "write_array"):
            assert report[phase].get("value_engine") != "mir_interpreter", report
            selected = list(callbacks(report[phase]))
            assert selected, (phase, report)
            assert all(n["callback"]["value_engine"] == "register_program"
                       for n in selected), (phase, selected)

    stem = ROOT / "tests/fixtures/gq_callback_shared_function"
    report = json.loads(subprocess.check_output(
        [dump_ops, str(stem.with_suffix(".tmir.sexp")),
         str(stem.with_suffix(".json")), "--execution-json"], text=True))
    selected = list(callbacks(report["write_array"]))
    assert Counter(n["operation"] for n in selected) == {"OP_ODE": 1, "OP_ALGEBRA_SOLVER": 1}
    for node in selected:
        assert node["variant"] == (0x10 if node["operation"] == "OP_ODE" else 0), node
        assert node.get("input_adjoint_mask", 0) == 0, node

    stem = ROOT / "tests/fixtures/matrix_callback_value_fallback"
    report = json.loads(subprocess.check_output(
        [dump_ops, str(stem.with_suffix(".tmir.sexp")),
         str(stem.with_suffix(".json")), "--execution-json"], text=True))
    assert report["write_array"]["value_engine"] == "mir_interpreter", report
    assert "compile time" in report["write_array"]["refusal"], report

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
