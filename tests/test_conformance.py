#!/usr/bin/env python3
"""Fast tests for the generated conformance harness itself."""

from __future__ import annotations

import contextlib
import dataclasses
import io
import json
import math
import os
import pathlib
import stat
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "harnesses"))

from conformance.aggregate import aggregate_reports, manifest_case_ids
from conformance.catalog import CatalogError, load_construct_catalog
from conformance.compare import (Expectation, Gate, Observation,
                                 compare_numeric, compare_observations,
                                 ulp_distance)
from conformance.construct_runner import (_construction_outcome,
                                          _profile_points,
                                          _stanli_transport_outcome)
from conformance.evaluate import evaluate_generated_case
from conformance.generate import (generated_inventory, make_scalar_shards,
                                  make_subshard_source, scalar_case_for,
                                  scalar_inventory)
from conformance.policy import PolicyError, load_policy
from conformance.protocol import JsonLinesClient, ProtocolError
from conformance.report import (ConformanceReport, ReportError, Scope,
                                SnapshotRefused, compare_snapshot,
                                render_markdown, snapshot_for, with_snapshot,
                                write_generated_sources, write_reproducers)
from conformance.runner import (Selection, partition_for, partition_manifest,
                                run_inventory)
from conformance.scalar_runner import _evaluate_shard, run_scalar_phase
from conformance.signatures import (ArrayType, DataType, FunctionType,
                                    SignatureParseError, TupleType,
                                    inventory_from_dump, load_inventory,
                                    parse_signature)
from conformance.status import (FINDING_STATUSES, CaseResult, ResultStatus)
from conformance.toml_compat import loads as toml_loads
import stan_conformance
from conformance.model_worker import (TransportUnavailable,
                                      _category as worker_category,
                                      _infrastructure_error, _stan_call)
from conformance.oracle import ReferenceBuild
from conformance.transport import transport_identity


DEFAULT_POLICY = REPO / "harnesses" / "conformance" / "policy.toml"
DEFAULT_CONSTRUCTS = REPO / "harnesses" / "conformance" / "constructs.toml"


def _write_policy(directory: pathlib.Path, body: str) -> pathlib.Path:
    path = directory / "policy.toml"
    path.write_text("schema_version = 1\npolicy_version = \"test\"\n" + body,
                    encoding="utf-8")
    return path


def _result(case_id: str, status: ResultStatus = ResultStatus.VERIFIED,
            reason: str = "exact parity", **kw) -> CaseResult:
    inventory_id = case_id.removeprefix("signature:") \
        if hasattr(str, "removeprefix") else (
            case_id[len("signature:"):] if case_id.startswith("signature:")
            else case_id)
    return CaseResult(case_id, inventory_id, inventory_id.split("(", 1)[0],
                      "signature", status, reason, **kw)


def _report(results, complete=True, total=None, **kw) -> ConformanceReport:
    total = len(results) if total is None else total
    return ConformanceReport(
        created_at="2026-08-12T00:00:00Z",
        inventory={"stanc_build_id": "stanc3 test", "raw_sha256": "abc",
                   "total_signatures": total, "total_names": total,
                   "raw_dump": ""},
        policy={"policy_version": "test", "sha256": "policy"},
        scope=Scope(complete, len(results), total),
        tools={"generator_version": "test"},
        results=tuple(results),
        **kw,
    )


class SignatureParserTests(unittest.TestCase):
    def test_scalar_canonicalization(self):
        signature = parse_signature("normal_lpdf(vector, real, vector) => real")
        self.assertEqual(signature.canonical_id,
                         "normal_lpdf(vector,real,vector)=>real")
        self.assertIsNone(signature.structural_inapplicability())

    def test_arrays_and_nested_arrays(self):
        signature = parse_signature(
            "f(array[,,] array[,] real, array[] int) => array[,] matrix")
        first = signature.arguments[0]
        self.assertIsInstance(first, ArrayType)
        self.assertEqual(first.rank, 3)
        self.assertIsInstance(first.element, ArrayType)
        self.assertEqual(first.element.rank, 2)
        self.assertEqual(signature.canonical_id,
                         "f(array[,,]array[,]real,array[]int)=>array[,]matrix")

    def test_tuple_and_complex(self):
        signature = parse_signature(
            "eigendecompose(complex_matrix) => "
            "tuple(complex_matrix, complex_vector)")
        self.assertIsInstance(signature.result, TupleType)
        self.assertTrue(signature.contains_complex)
        self.assertEqual(len(signature.result.elements), 2)

    def test_higher_order_data_qualifiers(self):
        signature = parse_signature(
            "integrate_1d((real, real, array[] real, data array[] real, "
            "data array[] int) => real, real, real, array[] real, "
            "array[] real, array[] int) => real")
        callback = signature.arguments[0]
        self.assertIsInstance(callback, FunctionType)
        self.assertIsInstance(callback.arguments[3], DataType)
        self.assertTrue(signature.has_function_argument)
        self.assertEqual(
            callback.canonical(),
            "(real,real,array[]real,data array[]real,data array[]int)=>real")

    def test_rng_and_integer_results_are_inapplicable(self):
        self.assertEqual(parse_signature("normal_rng(real, real) => real")
                         .structural_inapplicability(), "rng_only")
        self.assertEqual(parse_signature("int_step(real) => int")
                         .structural_inapplicability(),
                         "no_real_bearing_result")
        self.assertEqual(parse_signature("zeros_array(int) => array[] real")
                         .structural_inapplicability(),
                         "no_differentiable_path")

    def test_malformed_line_reports_source_position(self):
        with self.assertRaises(SignatureParseError) as caught:
            parse_signature("broken(array[,] real => real", line_number=17)
        self.assertIn("line 17", str(caught.exception))
        self.assertIn("^", str(caught.exception))

    def test_duplicate_inventory_is_fatal(self):
        with self.assertRaises(SignatureParseError) as caught:
            inventory_from_dump("f(real) => real\nf(real)=>real\n", "test")
        self.assertIn("duplicate canonical", str(caught.exception))

    def test_fake_stanc_inventory_process(self):
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw)
            script = root / "fake_stanc.py"
            script.write_text(
                "#!/usr/bin/env python3\n"
                "import sys\n"
                "if '--version' in sys.argv: print('stanc3 fake')\n"
                "elif '--dump-stan-math-signatures' in sys.argv:\n"
                " print('f(real) => real')\n"
                "else: sys.exit(2)\n", encoding="utf-8")
            if os.name == "nt":
                executable = root / "stanc.cmd"
                executable.write_text(
                    f'@"{sys.executable}" "{script}" %*\r\n',
                    encoding="utf-8")
            else:
                executable = script
                executable.chmod(executable.stat().st_mode | stat.S_IXUSR)
            inventory = load_inventory(executable)
            self.assertEqual(inventory.signature_count, 1)
            self.assertEqual(inventory.name_count, 1)
            self.assertEqual(inventory.stanc_build_id, "stanc3 fake")


class PolicyTests(unittest.TestCase):
    def test_toml_library_reads_multiline_arrays(self):
        parsed = toml_loads("""
schema_version = 1
names = [
  "one",
  "two", # trailing comments and commas are valid TOML
]
""")
        self.assertEqual(parsed["names"], ["one", "two"])

    def test_default_structural_rules_do_not_swallow_ordinary_real(self):
        policy = load_policy(DEFAULT_POLICY)
        ordinary = parse_signature("normal_lpdf(real, real, real) => real")
        complex_sig = parse_signature("abs(complex) => real")
        tuple_sig = parse_signature("qr(matrix) => tuple(matrix, matrix)")
        self.assertIsNone(policy.classification_for(ordinary))
        self.assertEqual(policy.classification_for(complex_sig).id,
                         "complex-types")
        self.assertEqual(policy.classification_for(tuple_sig).id,
                         "tuple-results")

    def test_broad_rules_cannot_swallow_ordinary_real_signatures(self):
        mutations = {
            "catch-all-name": """
[[classification]]
id = "swallow-everything"
reason = "mutation"
status = "expected_unsupported"
name_regex = ".*"
max_matches = 10
""",
            "structurally-unbounded": """
[[classification]]
id = "all-probability"
reason = "mutation"
probability = true
""",
            "negated-structural": """
[[classification]]
id = "ordinary-real"
reason = "mutation"
contains_complex = false
probability = true
""",
        }
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw)
            for label, body in mutations.items():
                with self.subTest(label=label), self.assertRaises(PolicyError):
                    load_policy(_write_policy(root, body))

    def test_named_rule_has_reviewed_match_ceiling(self):
        with tempfile.TemporaryDirectory() as raw:
            path = _write_policy(pathlib.Path(raw), """
[[classification]]
id = "two-overloads"
reason = "narrow product decision"
name = "f"
max_matches = 1
""")
            policy = load_policy(path)
            inventory = inventory_from_dump(
                "f(real) => real\nf(vector) => vector\n", "test")
            with self.assertRaises(PolicyError):
                policy.audit(inventory.signatures)

    def test_overlapping_rules_are_fatal(self):
        with tempfile.TemporaryDirectory() as raw:
            path = _write_policy(pathlib.Path(raw), """
[[classification]]
id = "complex"
reason = "complex"
contains_complex = true

[[classification]]
id = "tuple"
reason = "tuple"
return_kind = "tuple"
""")
            policy = load_policy(path)
            signature = parse_signature(
                "f(complex_matrix) => tuple(complex_matrix, real)")
            with self.assertRaises(PolicyError):
                policy.classification_for(signature)

    def test_default_gate_allows_ten_ulp(self):
        # Reviewed decision (2026-08-15, raised from 5 the same day): a
        # lane within 10 ULP of the reference is green. Anything looser
        # still needs a policy rule.
        import types
        from conformance.scalar_runner import _gate as scalar_gate
        policy = load_policy(DEFAULT_POLICY)
        normal = parse_signature("normal_lpdf(real, real, real) => real")
        case = types.SimpleNamespace(spec=types.SimpleNamespace(
            signature=normal))
        gate = scalar_gate(policy, case)
        self.assertEqual(gate.max_ulp, 10)
        self.assertTrue(compare_numeric(1.0, 1.0 + 10 * 2.0 ** -52,
                                        gate).agrees)
        self.assertFalse(compare_numeric(1.0, 1.0 + 11 * 2.0 ** -52,
                                         gate).agrees)

    def test_numeric_gate_is_narrow(self):
        policy = load_policy(DEFAULT_POLICY)
        ode = parse_signature("ode_rk45(real, real) => real")
        normal = parse_signature("normal_lpdf(real, real, real) => real")
        # O(rtol) discretization error each side; see the policy comment.
        self.assertEqual(policy.numeric_gate_for(ode).rel_tol, 1e-6)
        self.assertIsNone(policy.numeric_gate_for(normal))
        wiener = parse_signature(
            "wiener_lpdf(real, real, real, real, real) => real")
        self.assertEqual(policy.numeric_gate_for(wiener).rel_tol, 1e-12)
        self.assertEqual(policy.numeric_gate_named("iterative-ode-solvers"),
                         policy.numeric_gate_for(ode))
        with self.assertRaises(PolicyError):
            policy.numeric_gate_named("missing")


class ConstructCatalogTests(unittest.TestCase):
    def test_default_catalog_is_named_deterministic_and_file_backed(self):
        catalog = load_construct_catalog(DEFAULT_CONSTRUCTS, REPO)
        self.assertGreaterEqual(catalog.count, 20)
        self.assertGreaterEqual(catalog.category_count, 8)
        self.assertEqual(len({case.case_id for case in catalog.cases}),
                         catalog.count)
        self.assertTrue(all(case.source.is_file() and case.data.is_file()
                            for case in catalog.cases))
        merged = [case for case in catalog.cases
                  if case.source.name in {"data_and_tp_checks.stan",
                                          "structured_checks.stan"}]
        self.assertGreaterEqual(len(merged), 5)
        shape_rejection = next(
            case for case in catalog.cases
            if case.id == "declaration.data-shape-mismatch")
        self.assertEqual(shape_rejection.expected_exception,
                         "invalid_argument")
        again = load_construct_catalog(DEFAULT_CONSTRUCTS, REPO)
        self.assertEqual(catalog.sha256, again.sha256)

    def test_dynamic_point_profile_and_phase_classification(self):
        self.assertEqual(_profile_points("zeros-and-asymmetric", 3)[0],
                         (0.0, 0.0, 0.0))
        self.assertEqual(len(_profile_points("zeros-and-asymmetric", 3)), 3)
        catalog = load_construct_catalog(DEFAULT_CONSTRUCTS, REPO)
        case = next(case for case in catalog.cases
                    if case.id ==
                    "validation.transformed-parameter.scalar-reject")
        reference = {"accepted": True, "parameter_count": 1,
                     "parameter_names": ["x"]}
        stanli = {"accepted": False, "phase": "construction",
                  "exception_category": "domain_error", "message": "early"}
        outcome = _construction_outcome(case, reference, stanli)
        # A construction-time refusal is a coverage gap, not a wrong answer:
        # stanli declined at compile time rather than computing something
        # different, and the reason still names the phase it should have
        # rejected in. A disagreement about a value reaches MISMATCH through
        # the evaluation comparison instead.
        self.assertEqual(outcome.status, ResultStatus.UNEXPECTED_UNSUPPORTED)
        self.assertIn("cataloged evaluation phase", outcome.reason)

    def test_stanli_process_failure_is_an_implementation_finding(self):
        catalog = load_construct_catalog(DEFAULT_CONSTRUCTS, REPO)
        case = next(case for case in catalog.cases
                    if case.id == "phase.data.valid")
        outcome = _stanli_transport_outcome(
            case, {"accepted": True}, "evaluation",
            ProtocolError("stanli process exited before replying"))
        self.assertEqual(outcome.status,
                         ResultStatus.UNEXPECTED_UNSUPPORTED)
        self.assertIn("stanli transport failed", outcome.reason)

    def test_worker_normalizes_public_error_categories(self):
        self.assertEqual(worker_category(RuntimeError(
            "x does not satisfy lower constraint")), "domain_error")
        self.assertEqual(worker_category(RuntimeError(
            "dimension mismatch for x")), "invalid_argument")
        self.assertTrue(_infrastructure_error(ModuleNotFoundError("client")))
        self.assertTrue(_infrastructure_error(
            TransportUnavailable("facade missing")))
        protocol, diagnostics = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(protocol), \
                contextlib.redirect_stderr(diagnostics):
            _stan_call(print, "Stan says hello")
        self.assertEqual(protocol.getvalue(), "")
        self.assertEqual(diagnostics.getvalue(), "Stan says hello\n")

    def test_catalog_rejects_duplicate_ids(self):
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw)
            (root / "case.stan").write_text("model {}", encoding="utf-8")
            (root / "data.json").write_text("{}", encoding="utf-8")
            table = """
[[case]]
id = "same"
category = "test"
description = "duplicate"
source = "case.stan"
data = "data.json"
oracle_gate = "numeric"
point_profile = "zeros"
"""
            path = root / "constructs.toml"
            path.write_text(
                "schema_version=1\ncatalog_version='test'\n" + table + table,
                encoding="utf-8")
            with self.assertRaises(CatalogError):
                load_construct_catalog(path, root)

    def test_catalog_rejects_missing_fixture(self):
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw)
            path = root / "constructs.toml"
            path.write_text("""
schema_version = 1
catalog_version = "test"
[[case]]
id = "missing"
category = "test"
description = "missing source"
source = "absent.stan"
data = "absent.json"
oracle_gate = "numeric"
point_profile = "zeros"
""", encoding="utf-8")
            with self.assertRaises(CatalogError):
                load_construct_catalog(path, root)

    def test_census_row_requires_a_machine_readable_reason(self):
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw)
            (root / "case.stan").write_text("model {}", encoding="utf-8")
            (root / "data.json").write_text("{}", encoding="utf-8")
            path = root / "constructs.toml"
            path.write_text("""
schema_version = 1
catalog_version = "test"
[[case]]
id = "census"
category = "test"
description = "compile only"
source = "case.stan"
data = "data.json"
oracle_gate = "census"
""", encoding="utf-8")
            with self.assertRaisesRegex(CatalogError, "census_reason"):
                load_construct_catalog(path, root)


class NumericComparisonTests(unittest.TestCase):
    def test_exact_and_signed_zero(self):
        result = compare_numeric([-0.0, 1.0], [0.0, 1.0], Gate())
        self.assertTrue(result.agrees)
        self.assertEqual(ulp_distance(-0.0, 0.0), 0)

    def test_nonfinite_rules(self):
        self.assertTrue(compare_numeric(float("-inf"), float("-inf"),
                                        Gate()).agrees)
        self.assertFalse(compare_numeric(float("inf"), float("-inf"),
                                         Gate()).agrees)
        self.assertFalse(compare_numeric(float("nan"), float("nan"),
                                         Gate()).agrees)
        self.assertTrue(compare_numeric(float("nan"), float("nan"), Gate(),
                                        nan_paths=("lp",), root="lp").agrees)
        reference = Observation(True, float("-inf"), (0.0,))
        outcome = compare_observations(reference, reference, Expectation(),
                                       Gate())
        self.assertEqual(outcome.status, ResultStatus.GENERATOR_GAP)
        expected = Expectation(infinity_paths=("value",))
        self.assertEqual(compare_observations(
            reference, reference, expected, Gate()).status,
            ResultStatus.VERIFIED)
        decoded = Observation.from_dict({
            "accepted": True, "value": 0.0, "gradient": [],
            "outputs": ["Infinity", "-Infinity", "NaN"]})
        self.assertEqual(decoded.outputs[:2], [float("inf"), float("-inf")])
        self.assertTrue(math.isnan(decoded.outputs[2]))

    def test_shape_and_every_gradient_lane(self):
        self.assertFalse(compare_numeric([1.0, 2.0], [[1.0, 2.0]],
                                         Gate()).agrees)
        reference = Observation(True, 1.5, (0.1, 0.2, 0.3))
        got = Observation(True, 1.5, (0.1, 0.2, 0.4))
        outcome = compare_observations(got, reference, Expectation(), Gate())
        self.assertEqual(outcome.status, ResultStatus.MISMATCH)
        self.assertIn("gradient[2]", outcome.reason)

    def test_write_array_names_and_order_are_exact(self):
        reference = Observation(True, 1.0, (2.0,), (3.0,), ("x",))
        got = Observation(True, 1.0, (2.0,), (3.0,), ("y",))
        outcome = compare_observations(got, reference, Expectation(), Gate())
        self.assertEqual(outcome.status, ResultStatus.MISMATCH)
        self.assertIn("names", outcome.reason)

    def test_explicit_tolerance_still_reports_deviation(self):
        comparison = compare_numeric(1.0 + 5e-10, 1.0,
                                     Gate(max_ulp=None, rel_tol=1e-9))
        self.assertTrue(comparison.agrees)
        self.assertGreater(comparison.worst_relative, 0)
        self.assertGreater(comparison.worst_ulp, 0)

    def test_semantic_status_transitions(self):
        accepted = Observation(True, 1.0, (2.0,))
        rejected = Observation(False, phase="evaluation",
                               exception_category="domain_error")
        self.assertEqual(
            compare_observations(rejected, accepted, Expectation(), Gate()).status,
            ResultStatus.UNEXPECTED_UNSUPPORTED)
        self.assertEqual(
            compare_observations(rejected, rejected, Expectation(), Gate()).status,
            ResultStatus.GENERATOR_GAP)
        expected_rejection = Expectation(False, "evaluation", "domain_error")
        self.assertEqual(compare_observations(
            rejected, rejected, expected_rejection, Gate()).status,
            ResultStatus.VERIFIED)
        wrong_phase = Observation(False, phase="construction",
                                  exception_category="domain_error")
        self.assertEqual(compare_observations(
            wrong_phase, rejected, expected_rejection, Gate()).status,
            ResultStatus.MISMATCH)
        wrong_reference = Observation(False, phase="construction",
                                      exception_category="domain_error")
        self.assertEqual(compare_observations(
            wrong_reference, wrong_reference, expected_rejection,
            Gate()).status, ResultStatus.GENERATOR_GAP)


class GeneratorTests(unittest.TestCase):
    def test_scalar_generation_uses_one_lane_per_real_argument(self):
        signature = parse_signature("f(real, int, real) => real")
        spec = scalar_case_for(signature)
        self.assertIsNotNone(spec)
        self.assertEqual(spec.parameter_count, 2)
        call = spec.render_call(3)
        self.assertIn("theta[4]", call)
        self.assertIn("theta[5]", call)
        self.assertIn(", 2,", call)

    def test_probability_is_profiled_and_container_routes_elsewhere(self):
        probability = scalar_case_for(
            parse_signature("normal_lpdf(real, real, real) => real"))
        self.assertEqual(probability.domain_profile, "probability:normal")
        self.assertIn(" | ", probability.render_call(0))
        self.assertIsNone(scalar_case_for(
            parse_signature("sum(vector) => real")))

    def test_discrete_probability_outcome_stays_data(self):
        spec = scalar_case_for(
            parse_signature("binomial_lpmf(int, int, real) => real"))
        self.assertEqual(spec.parameter_count, 1)
        self.assertEqual(spec.centers, (3, 10, 0.4))

    def test_uniform_outcome_is_an_explicit_zero_partial(self):
        spec = scalar_case_for(
            parse_signature("uniform_lpdf(real, real, real) => real"))
        self.assertEqual(spec.expected_zero_lanes, (0,))

    def test_domain_profile_is_semantic_not_support_metadata(self):
        spec = scalar_case_for(
            parse_signature("log_diff_exp(real, real) => real"))
        self.assertEqual(spec.domain_profile, "ordered-pair")
        self.assertEqual(spec.centers, (1.5, 0.5))

    def test_shard_source_is_stable(self):
        inventory = inventory_from_dump(
            "z(real) => real\na(real, int, real) => real\n", "test")
        specs = scalar_inventory(inventory)
        first = make_scalar_shards(specs, shard_size=2)
        second = make_scalar_shards(tuple(reversed(specs)), shard_size=2)
        self.assertEqual(first, second)
        shard = first[0]
        self.assertEqual(shard.parameter_count, 3)
        self.assertEqual(len(shard.cases), 2)
        self.assertIn("else if (active_case == 2)", shard.source)

    def test_points_touch_only_active_case_lanes(self):
        inventory = inventory_from_dump(
            "a(real, real) => real\nz(real) => real\n", "test")
        shard = make_scalar_shards(scalar_inventory(inventory), 2)[0]
        first, second = shard.cases
        self.assertEqual(first.active_slice(first.points[1]), (0.3, -0.2))
        self.assertTrue(all(value == 0.0 for value in
                            first.points[1][second.parameter_offset:]))
        self.assertEqual(second.active_slice(second.points[2]), (-0.4,))
        retry = make_subshard_source((second,), shard.parameter_count)
        self.assertIn("int<lower=1, upper=1> active_case", retry)
        self.assertIn("vector[3] theta", retry)
        self.assertIn("theta[3]", retry)
        self.assertNotIn(first.case_id, retry)

    def test_invalid_shard_size_is_loud(self):
        with self.assertRaises(ValueError):
            make_scalar_shards((), 0)

    def test_elementwise_container_gets_independent_lanes_and_reduction(self):
        inventory = inventory_from_dump(
            "abs(real) => real\nabs(vector) => vector\n", "test")
        specs = generated_inventory(inventory)
        container = next(spec for spec in specs
                         if spec.inventory_id == "abs(vector)=>vector")
        self.assertEqual(container.parameter_count, 2)
        body = "\n".join(container.render_body(4, 1.0))
        self.assertIn("theta[5]", body)
        self.assertIn("theta[6]", body)
        self.assertIn("conformance_result[1]", body)
        self.assertIn("conformance_result[2]", body)

    def test_structural_container_without_scalar_analogue_stays_a_gap(self):
        inventory = inventory_from_dump("sum(vector) => real\n", "test")
        self.assertEqual(generated_inventory(inventory), ())

    def test_matrix_container_has_four_parameter_lanes(self):
        inventory = inventory_from_dump(
            "exp(real) => real\nexp(matrix) => matrix\n", "test")
        matrix = next(spec for spec in generated_inventory(inventory)
                      if spec.inventory_id == "exp(matrix)=>matrix")
        self.assertEqual(matrix.parameter_count, 4)
        self.assertIn("matrix[2, 2]", "\n".join(matrix.render_body(0, 1.0)))

    def test_recursive_array_and_vector_share_vectorization_axes(self):
        inventory = inventory_from_dump(
            "bessel_first_kind(int, real) => real\n"
            "bessel_first_kind(array[,] int, array[] vector) "
            "=> array[] vector\n", "test")
        spec = next(spec for spec in generated_inventory(inventory)
                    if spec.inventory_id.startswith(
                        "bessel_first_kind(array[,]"))
        self.assertEqual(len(spec.dimensions), 2)
        self.assertEqual(math.prod(spec.dimensions), 4)
        self.assertEqual(spec.parameter_count, 4)
        outer, inner = spec.dimensions
        body = "\n".join(spec.render_body(0, 1.0))
        self.assertIn(f"array[{outer}, {inner}] int", body)
        self.assertIn(f"array[{outer}] vector[{inner}]", body)
        self.assertIn("conformance_result", body)

    def test_mixed_depth_matrix_multiply_uses_compatible_square_shapes(self):
        inventory = inventory_from_dump(
            "multiply(real, real) => real\n"
            "multiply(matrix, vector) => vector\n", "test")
        spec = next(spec for spec in generated_inventory(inventory)
                    if spec.inventory_id.startswith("multiply(matrix"))
        self.assertEqual(spec.dimensions, (2, 2))
        body = "\n".join(spec.render_body(0, 1.0))
        self.assertIn("matrix[2, 2]", body)
        self.assertIn("vector[2]", body)

    def test_high_rank_recursive_case_stays_bounded(self):
        inventory = inventory_from_dump(
            "Phi(real) => real\n"
            "Phi(array[,,,,,,] matrix) => array[,,,,,,] matrix\n", "test")
        spec = next(spec for spec in generated_inventory(inventory)
                    if spec.inventory_id.startswith("Phi(array"))
        # Seven array axes plus two matrix axes, with one later lane total.
        self.assertEqual(len(spec.dimensions), 9)
        self.assertEqual(math.prod(spec.dimensions), 2)
        self.assertEqual(spec.parameter_count, 2)
        body = "\n".join(spec.render_body(0, 1.0))
        self.assertEqual(body.count(" * conformance_result"), 2)

    def test_vectorized_probability_profiles_every_real_leaf(self):
        inventory = inventory_from_dump(
            "normal_lpdf(real, real, real) => real\n"
            "normal_lpdf(array[] real, vector, array[] real) => real\n",
            "test")
        spec = next(spec for spec in generated_inventory(inventory)
                    if spec.inventory_id.startswith("normal_lpdf(array"))
        self.assertEqual(spec.parameter_count, 6)
        body = "\n".join(spec.render_body(2, 1.0))
        self.assertIn(" | ", body)
        for lane in range(3, 9):
            self.assertIn(f"theta[{lane}]", body)

    def test_vectorized_zero_partial_maps_to_every_outcome_lane(self):
        inventory = inventory_from_dump(
            "uniform_lpdf(real, real, real) => real\n"
            "uniform_lpdf(array[] real, real, real) => real\n", "test")
        spec = next(spec for spec in generated_inventory(inventory)
                    if spec.inventory_id.startswith("uniform_lpdf(array"))
        self.assertEqual(spec.expected_zero_lanes, (0, 1))


class GeneratedEvaluationTests(unittest.TestCase):
    class Client:
        def __init__(self, mutate=None):
            self.mutate = mutate

        def request(self, payload):
            point = list(payload["point"])
            gradient = [0.0] * len(point)
            # Test case occupies lanes 0 and 1.  Keep both informative at all
            # points, including the all-zero input.
            gradient[0:2] = [1.0, -2.0]
            value = point[0] - 2.0 * point[1]
            response = {"accepted": True, "value": value,
                        "gradient": gradient}
            if self.mutate:
                self.mutate(response)
            return response

    def case(self):
        spec = scalar_case_for(parse_signature("f(real, real) => real"))
        return make_scalar_shards((spec,), 1)[0].cases[0]

    def test_verified_case_checks_all_points(self):
        result = evaluate_generated_case(
            self.case(), self.Client(), self.Client(), Gate())
        self.assertEqual(result.status, ResultStatus.VERIFIED)
        self.assertTrue(result.probe_attempted)
        self.assertEqual(len(result.details["points"]), 3)

    def test_backend_process_failures_have_distinct_statuses(self):
        class FailingClient:
            def __init__(self, label):
                self.label = label

            def request(self, payload):
                raise ProtocolError(f"{self.label} process exited")

        scenarios = (
            (FailingClient("reference"), self.Client(),
             ResultStatus.HARNESS_ERROR, "reference"),
            (self.Client(), FailingClient("stanli"),
             ResultStatus.UNEXPECTED_UNSUPPORTED, "stanli"),
        )
        for reference, stanli, status, label in scenarios:
            with self.subTest(label=label):
                result = evaluate_generated_case(
                    self.case(), reference, stanli, Gate())
                self.assertEqual(result.status, status)
                self.assertIn(label, result.reason)

    def test_inactive_reference_dependency_is_generator_gap(self):
        # Put the case in a two-case shard so lane 2 is inactive.
        specs = (scalar_case_for(parse_signature("f(real, real) => real")),
                 scalar_case_for(parse_signature("z(real) => real")))
        case = make_scalar_shards(specs, 2)[0].cases[0]

        def bad_reference(response):
            response["gradient"][2] = 0.5

        result = evaluate_generated_case(
            case, self.Client(bad_reference), self.Client(), Gate())
        self.assertEqual(result.status, ResultStatus.GENERATOR_GAP)

    def test_report_records_active_slices_not_repeated_shard_vectors(self):
        specs = (scalar_case_for(parse_signature("f(real, real) => real")),
                 scalar_case_for(parse_signature("z(real) => real")))
        case = make_scalar_shards(specs, 2)[0].cases[0]
        result = evaluate_generated_case(
            case, self.Client(), self.Client(), Gate())
        point = result.details["points"][0]
        self.assertEqual(point["point"], [0.0, 0.0])
        self.assertEqual(point["point_total"], 3)
        self.assertEqual(point["reference"]["gradient"], [1.0, -2.0])
        self.assertEqual(point["reference"]["gradient_total"], 3)
        self.assertEqual(point["reference"]["gradient_encoding"],
                         "active_slice")

    def test_numeric_refusal_and_informativeness_mutations_turn_red(self):
        def bad_gradient(response):
            response["gradient"][1] += 0.25

        def bad_value(response):
            response["value"] += 0.125

        def refuse(response):
            response.clear()
            response.update(accepted=False, phase="construction",
                            exception_category="runtime_error",
                            message="deliberate compile refusal")

        def zero_second(response):
            response["gradient"][1] = 0.0

        scenarios = (
            ("wrong gradient", None, bad_gradient, ResultStatus.MISMATCH,
             "gradient"),
            ("wrong reference value", bad_value, None, ResultStatus.MISMATCH,
             "value"),
            ("compile refusal", None, refuse,
             ResultStatus.UNEXPECTED_UNSUPPORTED, "compile refusal"),
            ("zero reference lane", zero_second, zero_second,
             ResultStatus.GENERATOR_GAP, "uninformative"),
        )
        for label, reference_mutation, stanli_mutation, status, reason in scenarios:
            with self.subTest(label=label):
                result = evaluate_generated_case(
                    self.case(), self.Client(reference_mutation),
                    self.Client(stanli_mutation), Gate())
                self.assertEqual(result.status, status)
                self.assertIn(reason, result.reason)


class ReportAndSnapshotTests(unittest.TestCase):
    def test_command_line_baseline_is_opt_in(self):
        args = stan_conformance._parser().parse_args([])
        self.assertIsNone(args.baseline)
        with self.assertRaisesRegex(
                ReportError, "--update-snapshot requires --baseline PATH"):
            stan_conformance._update_configured_snapshot(
                _report([_result("signature:f(real)=>real")]), args)

    def _rejected(self, argv) -> str:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            with self.assertRaises(SystemExit):
                stan_conformance._parser().parse_args(argv)
        return stderr.getvalue()

    def test_selectors_are_single_use_and_mutually_exclusive(self):
        parser = stan_conformance._parser()
        self.assertEqual(parser.parse_args(["--case", "a"]).case, "a")
        self.assertEqual(parser.parse_args(["--filter", "b"]).filter, "b")
        # A second --case used to win silently, so `--case A --case B`
        # reported on B while reading as a claim about A.
        for argv in (["--case", "a", "--case", "b"],
                     ["--filter", "a", "--filter", "b"]):
            with self.subTest(argv=argv):
                self.assertIn("only once", self._rejected(argv))
        self.assertIn("not allowed", self._rejected(["--case", "a",
                                                     "--filter", "b"]))

    def test_snapshot_is_optional_but_explicit_snapshot_is_enforced(self):
        report = _report([_result("signature:f(real)=>real")])
        self.assertTrue(report.green)
        missing = with_snapshot(report, None, required=True)
        self.assertFalse(missing.green)
        self.assertTrue(missing.snapshot_delta.stale)
        snapshot = snapshot_for(report)
        checked = with_snapshot(report, snapshot, required=True)
        self.assertTrue(checked.green)
        self.assertTrue(checked.snapshot_delta.required)
        self.assertFalse(checked.snapshot_delta.stale)

    def test_disagreement_and_harness_failure_block(self):
        # The gate asks whether stanli answered differently, or whether the
        # harness failed to ask. Those two fail the run.
        for status in (ResultStatus.MISMATCH, ResultStatus.HARNESS_ERROR):
            with self.subTest(status=status):
                report = _report([_result("signature:f(real)=>real", status,
                                          "deliberate mutation")])
                self.assertFalse(report.green)
                self.assertTrue(any(issue.startswith(status.value)
                                    for issue in report.gate_issues))

    def test_coverage_gaps_are_reported_without_blocking(self):
        # An unimplemented function is a to-do, not a failure. It still has
        # to be counted, listed, and given a reproducer -- a gate that goes
        # quiet about the backlog is as useless as one that never goes green.
        for status in (ResultStatus.UNEXPECTED_UNSUPPORTED,
                       ResultStatus.GENERATOR_GAP):
            with self.subTest(status=status):
                report = _report([_result("signature:f(real)=>real", status,
                                          "not wired up yet")])
                self.assertTrue(report.green)
                self.assertEqual(report.gate_issues, ())
                self.assertEqual(report.status_counts[status.value], 1)
                self.assertIn(status, FINDING_STATUSES)

    def test_snapshot_refuses_partial_and_failed_runs(self):
        partial = _report([_result("signature:f(real)=>real")],
                          complete=False, total=2)
        with self.assertRaises(SnapshotRefused):
            snapshot_for(partial)
        failed = _report([_result("signature:f(real)=>real",
                                  ResultStatus.HARNESS_ERROR, "protocol died")])
        with self.assertRaises(SnapshotRefused):
            snapshot_for(failed)
        aggregate_error = _report(
            [_result("signature:f(real)=>real")],
            extra_gate_issues=("duplicate_case_ids:1",))
        with self.assertRaises(SnapshotRefused):
            snapshot_for(aggregate_error)

    def test_expected_unsupported_requires_reviewed_policy(self):
        with self.assertRaises(ValueError):
            _result("signature:f(real)=>real",
                    ResultStatus.EXPECTED_UNSUPPORTED, "not supported")
        result = _result(
            "signature:f(complex)=>real", ResultStatus.EXPECTED_UNSUPPORTED,
            "complex deferred", policy_rule="complex",
            policy_reason="complex deferred")
        snapshot = snapshot_for(_report([result]))
        self.assertEqual(snapshot["classifications"]["f(complex)=>real"]
                         ["policy_rule"], "complex")

    def test_snapshot_detects_removed_and_changed_inventory(self):
        initial = _report([_result("signature:f(real)=>real"),
                           _result("signature:g(real)=>real")])
        snapshot = snapshot_for(initial)
        changed = _report([_result("signature:f(real)=>real",
                                   ResultStatus.MISMATCH, "mutation")],
                          complete=False, total=2)
        delta = compare_snapshot(changed, snapshot)
        self.assertEqual(delta.changed_ids, ("f(real)=>real",))
        # Missing IDs are expected in a partial developer run and are checked
        # by the partial gate, not misreported as removed inventory.
        self.assertEqual(delta.missing_ids, ())

    def test_markdown_is_generated_and_deterministic(self):
        gap = _result("signature:f(real)=>real", ResultStatus.GENERATOR_GAP,
                      "generator missing", repro_command="run f")
        report = _report([gap])
        first = render_markdown(report)
        self.assertEqual(first, render_markdown(report))
        self.assertIn("do not edit", first)
        self.assertIn("generator missing", first)
        self.assertIn("run f", first)

    def test_reproducer_index_is_compact_and_probed_case_is_materialized(self):
        gap = _result("signature:plain(real)=>real",
                      ResultStatus.GENERATOR_GAP, "generator missing",
                      repro_command="run plain")
        probed = _result(
            "signature:f(real)=>real", ResultStatus.MISMATCH,
            "deliberate mismatch", repro_command="run f",
            probe_attempted=True,
            details={"active_case": 3,
                     "points": [{"point_index": 0, "point": [0.25]}]})
        with tempfile.TemporaryDirectory() as raw:
            target = pathlib.Path(raw) / "repro"
            write_reproducers(_report([gap, probed]), target)
            index = json.loads((target / "index.json").read_text(
                encoding="utf-8"))
            self.assertEqual(len(index), 2)
            self.assertNotIn("directory", index[1] if index[1]["case_id"]
                             == gap.case_id else index[0])
            probed_row = next(row for row in index
                              if row["case_id"] == probed.case_id)
            case_dir = target / probed_row["directory"]
            self.assertEqual(json.loads((case_dir / "data.json").read_text(
                encoding="utf-8")), {"active_case": 3})
            self.assertTrue((case_dir / "points.json").is_file())
            write_reproducers(_report([gap]), target)
            self.assertFalse(case_dir.exists())
            refreshed = json.loads((target / "index.json").read_text(
                encoding="utf-8"))
            self.assertEqual([row["case_id"] for row in refreshed],
                             [gap.case_id])

    def test_keep_materializes_each_generated_source_once(self):
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw)
            source = root / "shard.stan"
            source.write_text("parameters { real x; } model { target += x; }",
                              encoding="utf-8")
            first = _result(
                "signature:f(real)=>real", details={
                    "source_path": str(source),
                    "shard": {"id": "shard-one", "source_sha256": "abc"}})
            second = _result(
                "signature:g(real)=>real", details={
                    "source_path": str(source),
                    "shard": {"id": "shard-one", "source_sha256": "abc"}})
            report = dataclasses.replace(
                _report([first, second]), tools={"keep": True})
            target = root / "generated"
            write_generated_sources(report, target)
            index = json.loads((target / "index.json").read_text(
                encoding="utf-8"))
            self.assertEqual(len(index), 1)
            self.assertEqual(index[0]["case_ids"],
                             [first.case_id, second.case_id])
            report = dataclasses.replace(report, tools={"keep": False})
            write_generated_sources(report, target)
            self.assertFalse(target.exists())

    def test_red_cases_share_one_content_addressed_repro_source(self):
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw)
            source = root / "shard.stan"
            source.write_text("parameters { real x; } model { target += x; }",
                              encoding="utf-8")
            results = [
                _result(
                    f"signature:{name}(real)=>real", ResultStatus.MISMATCH,
                    "mutation", probe_attempted=True,
                    details={"source_path": str(source)})
                for name in ("f", "g")
            ]
            target = root / "repro"
            write_reproducers(_report(results), target)
            sources = list((target / "sources").glob("*.stan"))
            self.assertEqual(len(sources), 1)
            index = json.loads((target / "index.json").read_text(
                encoding="utf-8"))
            self.assertEqual(len({row["source"] for row in index}), 1)
            for row in index:
                pointer = json.loads(
                    (target / row["directory"] / "source.json").read_text(
                        encoding="utf-8"))
                self.assertTrue(pointer["sha256"].startswith(sources[0].stem))


class RunnerAndAggregateTests(unittest.TestCase):
    def setUp(self):
        self.inventory = inventory_from_dump(
            "f(real) => real\n"
            "abs(complex) => real\n"
            "normal_rng(real, real) => real\n", "stanc3 fake")
        self.policy = load_policy(DEFAULT_POLICY)

    def test_phase_one_classification_is_honest(self):
        report = run_inventory(
            self.inventory, self.policy, Selection(), pathlib.Path("stanc"),
            None, pathlib.Path("build"), REPO,
            created_at="2026-08-12T00:00:00Z")
        statuses = {result.family: result.status for result in report.results}
        self.assertEqual(statuses["f"], ResultStatus.GENERATOR_GAP)
        self.assertEqual(statuses["abs"], ResultStatus.EXPECTED_UNSUPPORTED)
        self.assertEqual(statuses["normal_rng"], ResultStatus.INAPPLICABLE)
        self.assertEqual(sum(report.status_counts.values()), 3)

    def test_selection_and_partitions_are_stable_and_exhaustive(self):
        ids = [signature.case_id for signature in self.inventory.signatures]
        for case_id in ids:
            self.assertEqual(partition_for(case_id, 7),
                             partition_for(case_id, 7))
        manifest = partition_manifest(self.inventory, 7)
        self.assertEqual(manifest["stanc_build_id"], "stanc3 fake")
        assigned = manifest_case_ids(manifest)
        self.assertEqual(set(assigned), set(ids))
        self.assertEqual(len(assigned), len(ids))
        selected = [signature for signature in self.inventory.signatures
                    if Selection(case="f(real)=>real").matches(signature)]
        self.assertEqual([signature.name for signature in selected], ["f"])

    def test_construct_catalog_rows_are_honest_generator_gaps(self):
        catalog = load_construct_catalog(DEFAULT_CONSTRUCTS, REPO)
        selection = Selection(case="construct:phase.data.valid")
        report = run_inventory(
            self.inventory, self.policy, selection, pathlib.Path("stanc"),
            None, pathlib.Path("build"), REPO,
            created_at="2026-08-12T00:00:00Z", catalog=catalog)
        self.assertEqual(len(report.results), 1)
        self.assertEqual(report.results[0].kind, "construct")
        self.assertEqual(report.results[0].status, ResultStatus.GENERATOR_GAP)
        self.assertEqual(report.scope.total_cases,
                         self.inventory.signature_count + catalog.count)
        manifest = partition_manifest(self.inventory, 4, catalog)
        self.assertEqual(manifest["total_constructs"], catalog.count)
        self.assertEqual(len(manifest_case_ids(manifest)),
                         self.inventory.signature_count + catalog.count)

    def test_missing_conformance_runtime_is_a_harness_error(self):
        inventory = inventory_from_dump("f(real) => real\n", "stanc3 fake")
        report = run_inventory(
            inventory, self.policy, Selection(), pathlib.Path("stanc"),
            pathlib.Path("cmdstan"), pathlib.Path("build"), REPO,
            created_at="2026-08-12T00:00:00Z")
        checked = run_scalar_phase(
            report, inventory, self.policy, pathlib.Path("cmdstan"),
            pathlib.Path("stanc"), pathlib.Path("build"),
            pathlib.Path("deps"), pathlib.Path("cache"),
            python_executable=pathlib.Path("/definitely/missing/python"))
        self.assertEqual(checked.results[0].status,
                         ResultStatus.HARNESS_ERROR)
        self.assertIn("runtime", checked.results[0].reason)

    def test_aggregate_rejects_missing_and_duplicate_results(self):
        inventory_meta = self.inventory.to_metadata()
        policy_meta = self.policy.to_metadata()
        one = ConformanceReport(
            "2026-08-12T00:00:00Z", inventory_meta, policy_meta,
            Scope(False, 1, 3, shard="1/3"), {},
            (_result("signature:f(real)=>real"),))
        two = ConformanceReport(
            "2026-08-12T00:00:00Z", inventory_meta, policy_meta,
            Scope(False, 2, 3, shard="2/3"), {},
            (_result("signature:abs(complex)=>real",
                     ResultStatus.EXPECTED_UNSUPPORTED, "complex deferred",
                     policy_rule="complex", policy_reason="complex deferred"),
             _result("signature:normal_rng(real,real)=>real",
                     ResultStatus.INAPPLICABLE, "rng")))
        manifest = {
            "schema_version": 1,
            "inventory_sha256": self.inventory.raw_sha256,
            "total_cases": 3,
            "partitions": [{"shard": "1/1", "count": 3,
                            "case_ids": [result.case_id
                                         for result in one.results + two.results]}],
        }
        merged = aggregate_reports([one, two], manifest,
                                   created_at="2026-08-12T00:00:00Z")
        self.assertTrue(merged.scope.complete)
        self.assertEqual(len(merged.results), 3)
        broken = aggregate_reports([one, one], manifest,
                                   created_at="2026-08-12T00:00:00Z")
        self.assertIn("duplicate_case_ids:1", broken.extra_gate_issues)
        self.assertIn("missing_case_ids:2", broken.extra_gate_issues)


class ProtocolSmokeTests(unittest.TestCase):
    SERVER = r"""
import json, sys
for line in sys.stdin:
    request = json.loads(line)
    case = request['case']
    value = 1.0 if case == 'one' else 2.0
    response = {'request_id': request['request_id'], 'accepted': True,
                'value': value, 'gradient': [value, -value]}
    print(json.dumps(response), flush=True)
"""

    POISON_WORKER = r"""
import argparse, json, pathlib, re, sys
p = argparse.ArgumentParser()
p.add_argument('model', type=pathlib.Path)
p.add_argument('--backend', required=True)
p.add_argument('--data')
a = p.parse_args()
source = a.model.read_text() if a.backend == 'stanli' else ''
poisoned = 'a_bad(' in source
lanes = [int(x) - 1 for x in re.findall(r'theta\[(\d+)\]', source)]
for line in sys.stdin:
    request = json.loads(line)
    response = {'request_id': request['request_id']}
    if request.get('action') == 'describe':
        if poisoned:
            response.update(accepted=False, phase='construction',
                            exception_category='runtime_error',
                            message='unsupported a_bad')
        else:
            response.update(accepted=True, parameter_count=2,
                            parameter_names=['theta.1', 'theta.2'])
    elif poisoned:
        response.update(accepted=False, phase='construction',
                        exception_category='runtime_error',
                        message='unsupported a_bad')
    else:
        point = request['point']
        lane = lanes[0] if lanes else request['active_case'] - 1
        gradient = [0.0] * len(point)
        gradient[lane] = 1.0
        response.update(accepted=True, value=point[lane], gradient=gradient)
    print(json.dumps(response), flush=True)
"""

    def test_two_case_fake_reference_and_mutation(self):
        with JsonLinesClient([sys.executable, "-u", "-c", self.SERVER],
                             timeout=3) as client:
            first = Observation.from_dict(client.request({"case": "one"}))
            second = Observation.from_dict(client.request({"case": "two"}))
        self.assertEqual(compare_observations(
            Observation(True, 1.0, (1.0, -1.0)), first,
            Expectation(), Gate()).status, ResultStatus.VERIFIED)
        # Deliberately perturb one gradient lane: the smoke oracle must turn
        # red as a mismatch, not pass because the log density still agrees.
        mutated = Observation(True, 2.0, (2.0, -1.5))
        outcome = compare_observations(mutated, second, Expectation(), Gate())
        self.assertEqual(outcome.status, ResultStatus.MISMATCH)
        self.assertIn("gradient[1]", outcome.reason)

    def test_protocol_id_mismatch_is_infrastructure_failure(self):
        server = ("import json,sys; r=json.loads(sys.stdin.readline()); "
                  "print(json.dumps({'request_id':999}), flush=True)")
        with JsonLinesClient([sys.executable, "-u", "-c", server],
                             timeout=3, label="stanli") as client:
            with self.assertRaisesRegex(
                    ProtocolError, "stanli response ID mismatch"):
                client.request({"case": "one"})

    def test_compile_poison_is_bisected_without_harming_its_neighbor(self):
        inventory = inventory_from_dump(
            "a_bad(real) => real\nz_good(real) => real\n", "test")
        shard = make_scalar_shards(scalar_inventory(inventory), 2)[0]
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw)
            source = root / "shard.stan"
            source.write_text(shard.source, encoding="utf-8")
            dummy = root / "reference.so"
            dummy.touch()
            worker = root / "worker.py"
            worker.write_text(self.POISON_WORKER, encoding="utf-8")
            results = _evaluate_shard(
                shard, ReferenceBuild(dummy, None, source,
                                      shard.source_sha256),
                load_policy(DEFAULT_POLICY), pathlib.Path("stanc"),
                pathlib.Path("cmdstan"), pathlib.Path("build"),
                pathlib.Path(sys.executable), None, 3.0, worker)
        by_id = {result.inventory_id: result for result in results}
        self.assertEqual(by_id["a_bad(real)=>real"].status,
                         ResultStatus.UNEXPECTED_UNSUPPORTED)
        supported = by_id["z_good(real)=>real"]
        self.assertEqual(supported.status, ResultStatus.VERIFIED)
        self.assertTrue(supported.details["stanli_shard"]["retry"])

    def test_transport_identity_records_public_client_versions(self):
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw)
            (root / "stanli.py").write_text(
                "__version__='test-stanli'\n"
                "def build_id(): return 'test-build'\n"
                "def stan_to_mir(source): return source\n"
                "def bridgestan_model(): pass\n", encoding="utf-8")
            (root / "bridgestan.py").write_text(
                "__version__='test-bridge'\n", encoding="utf-8")
            identity = transport_identity(
                pathlib.Path(sys.executable), root, "test-bridge")
        self.assertEqual(identity["stanli_build_id"], "test-build")
        self.assertEqual(identity["bridgestan_version"], "test-bridge")


if __name__ == "__main__":
    unittest.main()
