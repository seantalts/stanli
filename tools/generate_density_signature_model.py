#!/usr/bin/env python3
"""Generate exhaustive probability-function execution fixtures.

The unified runtime registry is joined to stanc's authoritative signature
inventory. Every compatible registered density, mass, CDF, LCDF, and LCCDF
overload is emitted once, in models of a fixed number of calls, called from
transformed data, the ordinary model graph, runtime control, and generated
quantities.
"""

from __future__ import annotations

import argparse
import functools
import pathlib
import re
import sys
from typing import Callable


ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "harnesses"))

from conformance.signatures import (  # noqa: E402
    ArrayType,
    AtomicType,
    Signature,
    StanType,
    load_inventory,
)
from function_signature_common import (  # noqa: E402
    all_context_model,
    by_reason,
    generated_model_record,
    numeric_leaf_kind,
    partitions,
    portable_build_id,
    registry_by_name,
    resolve_registry_spec,
    unwrap_data,
    write_generated_outputs,
)


DEFAULT_STANC = ROOT / "deps/stanc3/stanc"
DEFAULT_REGISTRY = ROOT / "build/dump_function_specs"
DEFAULT_OUTPUT_DIR = ROOT / "tests/fixtures"
DEFAULT_MANIFEST = ROOT / "tests/function_coverage/density_signatures_manifest.json"
FILE_GLOB = "density_signatures_*.stan"
CASES_PER_MODEL = 1440
PROBABILITY_SUFFIXES = ("_lpdf", "_lpmf", "_lccdf", "_lcdf", "_cdf")


def probability_base(name: str) -> str:
    for suffix in PROBABILITY_SUFFIXES:
        if name.endswith(suffix):
            return name.removesuffix(suffix)
    raise ValueError(f"registered probability function has no known suffix: {name}")


def surface_type(value: StanType) -> str:
    value, _ = unwrap_data(value)
    if isinstance(value, AtomicType):
        return value.name
    if isinstance(value, ArrayType) and value.rank == 1:
        return "array[] " + surface_type(value.element)
    raise ValueError(f"unsupported density test type {value.canonical()}")


def density_signatures(stanc: pathlib.Path, registry: pathlib.Path) \
        -> tuple[list[Signature], list[dict[str, str]], object]:
    inventory = load_inventory(stanc)
    specs = registry_by_name(registry, "density")
    selected: list[Signature] = []
    excluded: list[dict[str, str]] = []
    for signature in inventory.signatures:
        candidates = specs.get(signature.name)
        if candidates is None:
            continue
        descriptor, reason = resolve_registry_spec(
            candidates,
            tuple(numeric_leaf_kind(value) for value in signature.arguments),
            signature.result.canonical(), signature.canonical_id)
        if descriptor is not None:
            selected.append(signature)
        else:
            excluded.append({"signature": signature.canonical_id,
                             "reason": reason})
    selected.sort(key=lambda value: value.canonical_id)
    return selected, excluded, inventory


def role(name: str, index: int) -> str:
    """Return a support-safe semantic role for a density argument."""
    base = probability_base(name)
    # Suffix-specific repairs. Two keep the log density finite, since one
    # nonfinite case would absorb every other case's contribution to the
    # partition's log density and gradient; the beta_binomial CDFs get a
    # cheap series (see r_shallow).
    # bernoulli_lccdf(1 | p) is log P(Y > 1) = log 0 = -inf identically, so
    # its outcome must sit below the top of the support. The student_t CDF
    # family's degrees-of-freedom derivative is 0/0 at y == mu (CmdStan
    # reports the same nan), and the generic roles render y and mu from the
    # same expression, so y gets an offset value instead.
    overrides = {
        "bernoulli_lccdf": ("count_zero", "prob"),
        "student_t_cdf": ("tail_y", "df", "any", "positive"),
        "student_t_lcdf": ("tail_y", "df", "any", "positive"),
        "student_t_lccdf": ("tail_y", "df", "any", "positive"),
        "beta_binomial_cdf": ("count", "trials", "shallow", "shallow"),
        "beta_binomial_lcdf": ("count", "trials", "shallow", "shallow"),
        "beta_binomial_lccdf": ("count", "trials", "shallow", "shallow"),
    }
    if name in overrides:
        try:
            return overrides[name][index]
        except IndexError as exc:
            raise ValueError(
                f"no argument roles for {name} argument {index}") from exc
    roles = {
        "bernoulli": ("count", "prob"),
        "bernoulli_logit": ("count", "any"),
        "bernoulli_logit_glm": ("count", "design", "any", "any"),
        "beta_binomial": ("count", "trials", "positive", "positive"),
        "beta": ("prob", "positive", "positive"),
        "beta_neg_binomial": ("count", "positive", "steep", "positive"),
        "beta_proportion": ("prob", "prob", "positive"),
        "binomial": ("count", "trials", "prob"),
        "binomial_logit": ("count", "trials", "any"),
        "binomial_logit_glm": ("count", "trials", "design", "any", "any"),
        "categorical": ("category", "simplex"),
        "categorical_logit": ("category", "any"),
        "categorical_logit_glm": ("category", "design", "any", "design"),
        "cauchy": ("any", "any", "positive"),
        "chi_square": ("positive", "df"),
        "dirichlet": ("simplex", "positive"),
        "dirichlet_multinomial": ("count", "positive"),
        "discrete_range": ("range_value", "range_lower", "range_upper"),
        "double_exponential": ("any", "any", "positive"),
        "exp_mod_normal": ("any", "any", "positive", "positive"),
        "exponential": ("positive", "positive"),
        "frechet": ("positive", "positive", "positive"),
        "gamma": ("positive", "positive", "positive"),
        "gumbel": ("any", "any", "positive"),
        "hypergeometric": ("hyper_n", "hyper_N", "hyper_a", "hyper_b"),
        "inv_chi_square": ("positive", "df"),
        "inv_gamma": ("positive", "positive", "positive"),
        "inv_wishart": ("spd", "df", "spd"),
        "inv_wishart_cholesky": ("chol", "df", "chol"),
        "lkj_corr": ("corr", "positive"),
        "lkj_corr_cholesky": ("corr_chol", "positive"),
        "lkj_cov": ("spd", "positive", "positive", "positive"),
        "logistic": ("any", "any", "positive"),
        "loglogistic": ("positive", "positive", "positive"),
        "lognormal": ("positive", "any", "positive"),
        "multi_gp": ("design", "spd", "positive"),
        "multi_gp_cholesky": ("design", "chol", "positive"),
        "multi_normal": ("any", "any", "spd"),
        "multi_normal_prec": ("any", "any", "spd"),
        "multi_normal_cholesky": ("any", "any", "chol"),
        "multi_student_t": ("any", "df", "any", "spd"),
        "multi_student_t_cholesky": ("any", "df", "any", "chol"),
        "multinomial": ("count", "simplex"),
        "multinomial_logit": ("count", "any"),
        "neg_binomial": ("count", "positive", "positive"),
        "neg_binomial_2": ("count", "positive", "positive"),
        "neg_binomial_2_log": ("count", "any", "positive"),
        "neg_binomial_2_log_glm": ("count", "design", "any", "any", "positive"),
        "normal": ("any", "any", "positive"),
        "normal_id_glm": ("any", "design", "any", "any", "positive"),
        "ordered_logistic": ("category", "any", "cutpoints"),
        "ordered_logistic_glm": ("category", "design", "any", "cutpoints"),
        "ordered_probit": ("category", "any", "cutpoints"),
        "pareto": ("pareto_y", "pareto_min", "positive"),
        "pareto_type_2": ("pareto2_y", "pareto2_min", "positive", "positive"),
        "poisson": ("count", "positive"),
        "poisson_binomial": ("count_zero", "prob"),
        "poisson_log": ("count", "any"),
        "poisson_log_glm": ("count", "design", "any", "any"),
        "rayleigh": ("positive", "positive"),
        "scaled_inv_chi_square": ("positive", "df", "positive"),
        "skew_double_exponential": ("any", "any", "positive", "prob"),
        "skew_normal": ("any", "any", "positive", "any"),
        "std_normal": ("any",),
        "student_t": ("any", "df", "any", "positive"),
        "uniform": ("uniform_y", "uniform_lower", "uniform_upper"),
        "von_mises": ("any", "any", "positive"),
        "weibull": ("positive", "positive", "positive"),
        "wiener": ("wiener_y", "wiener_alpha", "wiener_tau", "prob", "any"),
        "wishart": ("spd", "df", "spd"),
        "wishart_cholesky": ("chol", "df", "chol"),
        "yule_simon": ("count", "positive"),
    }
    try:
        return roles[base][index]
    except (KeyError, IndexError) as exc:
        raise ValueError(f"no argument roles for {name} argument {index}") from exc


def expression(type_name: str, semantic_role: str) -> str:
    integer = {
        "count": "i_count", "count_zero": "i_count_zero",
        "category": "i_category", "trials": "i_trials",
        "positive": "i_positive", "steep": "i_steep",
        "range_value": "i_range_value", "range_lower": "i_range_lower",
        "range_upper": "i_range_upper", "hyper_n": "i_hyper_n",
        "hyper_N": "i_hyper_N", "hyper_a": "i_hyper_a", "hyper_b": "i_hyper_b",
    }
    if type_name == "int":
        return integer[semantic_role]
    if type_name == "array[] int":
        return "a_" + integer[semantic_role]

    scalar_role = {
        "any": "r_any", "design": "r_any", "positive": "r_positive",
        "steep": "r_steep", "shallow": "r_shallow",
        "df": "r_df", "prob": "r_prob", "simplex": "r_prob",
        "uniform_y": "r_uniform_y", "uniform_lower": "r_uniform_lower",
        "uniform_upper": "r_uniform_upper", "pareto_y": "r_pareto_y",
        "pareto_min": "r_pareto_min", "pareto2_y": "r_pareto2_y",
        "pareto2_min": "r_pareto2_min", "wiener_y": "r_wiener_y",
        "wiener_alpha": "r_wiener_alpha", "wiener_tau": "r_wiener_tau",
        "tail_y": "r_tail_y",
    }
    matrix_role = {
        "design": "m_design", "any": "m_design", "positive": "m_positive",
        "spd": "m_spd", "chol": "m_chol", "corr": "m_corr",
        "corr_chol": "m_corr_chol",
    }
    if type_name == "real":
        return scalar_role[semantic_role]
    if type_name == "matrix":
        return matrix_role[semantic_role]

    prefix = {
        "vector": "v_", "row_vector": "rv_", "array[] real": "a_r_",
        "array[] vector": "a_v_", "array[] row_vector": "a_rv_",
    }[type_name]
    container_role = {
        "any": "any", "design": "any", "positive": "positive",
        "steep": "steep", "shallow": "shallow", "df": "df",
        "prob": "prob", "simplex": "simplex", "cutpoints": "cutpoints",
        "uniform_y": "uniform_y", "uniform_lower": "uniform_lower",
        "uniform_upper": "uniform_upper", "pareto_y": "pareto_y",
        "pareto_min": "pareto_min", "pareto2_y": "pareto2_y",
        "pareto2_min": "pareto2_min", "wiener_y": "wiener_y",
        "wiener_alpha": "wiener_alpha", "wiener_tau": "wiener_tau",
        "tail_y": "tail_y",
    }
    return prefix + container_role[semantic_role]


BODY_DECLARATIONS = """    int i_count = 1;
    int i_count_zero = 0;
    int i_category = 1;
    int i_trials = 2;
    int i_positive = 2;
    int i_steep = 6;
    int i_range_value = 1;
    int i_range_lower = 0;
    int i_range_upper = 2;
    int i_hyper_n = 1;
    int i_hyper_N = 2;
    int i_hyper_a = 2;
    int i_hyper_b = 2;
    array[2] int a_i_count = {1, 1};
    array[2] int a_i_count_zero = {0, 0};
    array[2] int a_i_category = {1, 2};
    array[2] int a_i_trials = {2, 2};
    array[2] int a_i_positive = {2, 2};
    array[2] int a_i_steep = {6, 6};
    array[2] int a_i_range_value = {1, 1};
    array[2] int a_i_range_lower = {0, 0};
    array[2] int a_i_range_upper = {2, 2};
    array[2] int a_i_hyper_n = {1, 1};
    array[2] int a_i_hyper_N = {2, 2};
    array[2] int a_i_hyper_a = {2, 2};
    array[2] int a_i_hyper_b = {2, 2};

    real r_any = 0.2 + 0.01 * seed;
    // Stay away from positive integers where beta_neg_binomial_cdf's
    // hypergeometric representation can contain singular denominator terms.
    real r_positive = 1.2 + exp(0.01 * seed);
    // beta_neg_binomial's alpha sets how fast its CDF series converge: the
    // 3F2 tail decays like k^-alpha and Boost's pFq at z = 1 sums to machine
    // precision, so a call costs 19 ms at alpha 2.2 and 0.07 ms at 6.2.
    real r_steep = 5.2 + exp(0.01 * seed);
    // beta_binomial's CDF sums a 3F2 whose terms grow like k^(alpha+beta-2)
    // until a 1e-6 log tolerance, so alpha + beta >= 2 runs every one of
    // its 1e5 steps; well below 2 the series stops within a few.
    real r_shallow = 0.15 + 0.01 * seed;
    real r_df = 3 + exp(0.01 * seed);
    real r_prob = inv_logit(0.01 * seed);
    real r_uniform_y = 0.01 * seed;
    real r_uniform_lower = -2 - exp(0.01 * seed);
    real r_uniform_upper = 2 + exp(0.01 * seed);
    real r_pareto_y = 2 + exp(0.01 * seed);
    real r_pareto_min = exp(0.01 * seed);
    real r_pareto2_y = 2 + exp(0.01 * seed);
    real r_pareto2_min = -exp(0.01 * seed);
    real r_wiener_y = 1.5 + 0.01 * seed;
    real r_wiener_alpha = 1.2 + 0.01 * seed;
    real r_wiener_tau = 0.2 + 0.001 * seed;
    real r_tail_y = 0.7 + 0.01 * seed;

    vector[2] v_any = [r_any, r_any + 0.1]';
    vector[2] v_positive = [r_positive, r_positive + 0.1]';
    vector[2] v_steep = [r_steep, r_steep + 0.1]';
    vector[2] v_shallow = [r_shallow, r_shallow + 0.1]';
    vector[2] v_df = [r_df, r_df + 0.1]';
    vector[2] v_prob = [r_prob, inv_logit(0.02 * seed)]';
    vector[2] v_simplex = softmax([0.01 * seed, 0]');
    vector[2] v_cutpoints = [-1 + 0.01 * seed, 1 + 0.01 * seed]';
    vector[2] v_uniform_y = rep_vector(r_uniform_y, 2);
    vector[2] v_uniform_lower = rep_vector(r_uniform_lower, 2);
    vector[2] v_uniform_upper = rep_vector(r_uniform_upper, 2);
    vector[2] v_pareto_y = rep_vector(r_pareto_y, 2);
    vector[2] v_pareto_min = rep_vector(r_pareto_min, 2);
    vector[2] v_pareto2_y = rep_vector(r_pareto2_y, 2);
    vector[2] v_pareto2_min = rep_vector(r_pareto2_min, 2);
    vector[2] v_wiener_y = [r_wiener_y, r_wiener_y + 0.05]';
    vector[2] v_wiener_alpha = [r_wiener_alpha, r_wiener_alpha + 0.05]';
    vector[2] v_wiener_tau = [r_wiener_tau, r_wiener_tau + 0.01]';
    vector[2] v_tail_y = [r_tail_y, r_tail_y + 0.1]';
    vector[1] v_any_one = [r_any]';
    vector[1] v_positive_one = [r_positive]';
    row_vector[2] rv_any = [r_any, r_any + 0.1];
    row_vector[2] rv_positive = [r_positive, r_positive + 0.1];
    row_vector[2] rv_steep = [r_steep, r_steep + 0.1];
    row_vector[2] rv_shallow = [r_shallow, r_shallow + 0.1];
    row_vector[2] rv_df = [r_df, r_df + 0.1];
    row_vector[2] rv_prob = [r_prob, inv_logit(0.02 * seed)];
    row_vector[2] rv_simplex = [r_prob, 1 - r_prob];
    row_vector[2] rv_cutpoints = [-1 + 0.01 * seed, 1 + 0.01 * seed];
    row_vector[2] rv_uniform_y = [r_uniform_y, r_uniform_y];
    row_vector[2] rv_uniform_lower = [r_uniform_lower, r_uniform_lower];
    row_vector[2] rv_uniform_upper = [r_uniform_upper, r_uniform_upper];
    row_vector[2] rv_pareto_y = [r_pareto_y, r_pareto_y];
    row_vector[2] rv_pareto_min = [r_pareto_min, r_pareto_min];
    row_vector[2] rv_pareto2_y = [r_pareto2_y, r_pareto2_y];
    row_vector[2] rv_pareto2_min = [r_pareto2_min, r_pareto2_min];
    row_vector[2] rv_wiener_y = [r_wiener_y, r_wiener_y + 0.05];
    row_vector[2] rv_wiener_alpha = [r_wiener_alpha, r_wiener_alpha + 0.05];
    row_vector[2] rv_wiener_tau = [r_wiener_tau, r_wiener_tau + 0.01];
    row_vector[2] rv_tail_y = [r_tail_y, r_tail_y + 0.1];

    array[2] real a_r_any = {r_any, r_any + 0.1};
    array[2] real a_r_positive = {r_positive, r_positive + 0.1};
    array[2] real a_r_steep = {r_steep, r_steep + 0.1};
    array[2] real a_r_shallow = {r_shallow, r_shallow + 0.1};
    array[2] real a_r_df = {r_df, r_df + 0.1};
    array[2] real a_r_prob = {r_prob, inv_logit(0.02 * seed)};
    array[2] real a_r_uniform_y = {r_uniform_y, r_uniform_y};
    array[2] real a_r_uniform_lower = {r_uniform_lower, r_uniform_lower};
    array[2] real a_r_uniform_upper = {r_uniform_upper, r_uniform_upper};
    array[2] real a_r_pareto_y = {r_pareto_y, r_pareto_y};
    array[2] real a_r_pareto_min = {r_pareto_min, r_pareto_min};
    array[2] real a_r_pareto2_y = {r_pareto2_y, r_pareto2_y};
    array[2] real a_r_pareto2_min = {r_pareto2_min, r_pareto2_min};
    array[2] real a_r_wiener_y = {r_wiener_y, r_wiener_y + 0.05};
    array[2] real a_r_wiener_alpha = {r_wiener_alpha, r_wiener_alpha + 0.05};
    array[2] real a_r_wiener_tau = {r_wiener_tau, r_wiener_tau + 0.01};
    array[2] real a_r_tail_y = {r_tail_y, r_tail_y + 0.1};

    array[2] vector[2] a_v_any = {v_any, v_any};
    array[2] vector[2] a_v_positive = {v_positive, v_positive};
    array[2] vector[2] a_v_steep = {v_steep, v_steep};
    array[2] vector[2] a_v_simplex = {v_simplex, v_simplex};
    array[2] vector[2] a_v_cutpoints = {v_cutpoints, v_cutpoints};
    array[2] row_vector[2] a_rv_any = {rv_any, rv_any};
    array[2] row_vector[2] a_rv_positive = {rv_positive, rv_positive};
    array[2] row_vector[2] a_rv_steep = {rv_steep, rv_steep};
    array[2] row_vector[2] a_rv_simplex = {rv_simplex, rv_simplex};

    matrix[2, 2] m_design = [[1, r_any], [r_any, 1]];
    matrix[2, 2] m_positive = rep_matrix(r_positive, 2, 2);
    matrix[2, 2] m_chol = diag_matrix(rep_vector(r_positive, 2));
    matrix[2, 2] m_spd = multiply_lower_tri_self_transpose(m_chol);
    real corr = tanh(0.01 * seed);
    matrix[2, 2] m_corr_chol = [[1, 0], [corr, sqrt(1 - square(corr))]];
    matrix[2, 2] m_corr = multiply_lower_tri_self_transpose(m_corr_chol);
"""


# Role variables, longest prefix first so `a_r_any` never half-matches `r_`.
ROLE_VARIABLE = re.compile(
    r"\b(a_rv_|a_i_|a_r_|a_v_|rv_|i_|r_|v_|m_|corr\b)")
IDENTIFIER = re.compile(r"\b[A-Za-z_]\w*\b")


@functools.lru_cache(maxsize=None)
def declarations() -> dict[str, str]:
    """Role variable name -> its declaration, in BODY_DECLARATIONS order."""
    result = {}
    for line in BODY_DECLARATIONS.splitlines():
        line = line.strip()
        if not line or line.startswith("//"):
            continue
        head, _ = line.split(" = ", 1)
        result[head.split()[-1]] = line
    return result


def data_twin_declarations() -> str:
    """BODY_DECLARATIONS with every role variable renamed d_* and seed
    replaced by a literal zero. A literal-only local constant-folds to a
    data argument in both runtimes, which is what lets one call activate a
    single argument while every other one takes the data instantiation."""
    body = "".join(f"    {line}\n" for line in declarations().values())
    body = re.sub(r"\bseed\b", "0.0", body)
    return ROLE_VARIABLE.sub(lambda m: "d_" + m.group(1), body)


@functools.lru_cache(maxsize=None)
def seed_dependent() -> dict[str, set[str]]:
    """Seed-dependent role variables -> the role variables they reference."""
    uses = {name: set(IDENTIFIER.findall(line.split(" = ", 1)[1]))
            for name, line in declarations().items()}
    dependent = {}
    for name, used in uses.items():
        if "seed" in used or used & set(dependent):
            dependent[name] = used & set(dependent)
    return dependent


def prelude() -> str:
    dependent = seed_dependent()
    hoisted = "".join(f"    {line}\n"
                      for name, line in declarations().items()
                      if name not in dependent)
    return data_twin_declarations() + hoisted


def case_declarations(arguments: list[str]) -> list[str]:
    dependent = seed_dependent()
    needed = set()
    pending = [name for argument in arguments
               for name in IDENTIFIER.findall(argument)]
    while pending:
        name = pending.pop()
        if name in dependent and name not in needed:
            needed.add(name)
            pending.extend(dependent[name])
    return [line for name, line in declarations().items() if name in needed]


def as_data(argument: str) -> str:
    return ROLE_VARIABLE.sub(lambda m: "d_" + m.group(1), argument)


def real_argument_positions(signature: Signature) -> list[int]:
    """Indices whose surface type carries real values (gradient-capable)."""
    return [index for index, value in enumerate(signature.arguments)
            if "int" not in surface_type(value)]


def render_case(signature: Signature, active_index: int | None,
                position: int) -> str:
        name = signature.name
        argument_types = [surface_type(value) for value in signature.arguments]
        arguments = [
            expression(type_name, role(name, index))
            for index, type_name in enumerate(argument_types)
        ]
        base = probability_base(name)
        design_arguments = {
            "bernoulli_logit_glm": (1, (0, 2)),
            "binomial_logit_glm": (2, (0, 1, 3)),
            "categorical_logit_glm": (1, (0,)),
            "neg_binomial_2_log_glm": (1, (0, 2, 4)),
            "normal_id_glm": (1, (0, 2, 4)),
            "ordered_logistic_glm": (1, (0,)),
            "poisson_log_glm": (1, (0, 2)),
        }
        if base in design_arguments:
            design_index, per_observation = design_arguments[base]
            if argument_types[design_index] == "row_vector":
                for index in per_observation:
                    type_name = argument_types[index]
                    if type_name == "vector":
                        one_vector = {
                            "any": "v_any_one",
                            "positive": "v_positive_one",
                        }
                        arguments[index] = one_vector[role(name, index)]
                    elif type_name == "array[] int":
                        arguments[index] = "{" + expression("int", role(name, index)) + "}"
                    elif type_name == "array[] real":
                        arguments[index] = "{" + expression("real", role(name, index)) + "}"
        if base == "normal_id_glm" and argument_types[1] == "vector":
            arguments[3] = "v_any_one"
        # The scalar location overload broadcasts only when the cutpoints are
        # shared. If the cutpoints themselves use the array overload, make it
        # a one-observation array while retaining the exact language types.
        if (base == "ordered_probit" and argument_types[1] == "real" and
                argument_types[2] == "array[] vector"):
            arguments[0] = "{i_category}"
            arguments[2] = "{v_cutpoints}"
        # Per-argument activity: every other argument takes its constant
        # data twin, so the call reaches the same single-active
        # instantiation CmdStan's generated code selects.
        identity = signature.canonical_id
        if active_index is not None:
            arguments = [argument if index == active_index
                         else as_data(argument)
                         for index, argument in enumerate(arguments)]
            identity += f"@{active_index}"
        # Stan requires probability-function syntax for densities, mass
        # functions, and every CDF spelling alike.
        call = f"{name}({arguments[0]} | {', '.join(arguments[1:])})"
        lines = ([f"real seed = probe[{position}];"]
                 + case_declarations(arguments)
                 + [f"out[{position}] = {call};"])
        return (f"    {{  // {identity}\n"
                + "".join(f"      {line}\n" for line in lines)
                + "    }\n")


def render_model(index: int, count: int,
                 cases: list[tuple[str, Callable[[int], str]]]) -> str:
    function = f"density_signatures_{index:02d}"
    body = prelude() + "".join(
        render(position) for position, (_, render) in enumerate(cases, 1))
    return all_context_model(
        function, len(cases), body.rstrip("\n"),
        "Generated by tools/generate_density_signature_model.py from the "
        "unified FunctionSpec registry",
        f"Partition {index} of {count}; {len(cases)} overload instantiations.")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stanc", type=pathlib.Path, default=DEFAULT_STANC)
    parser.add_argument("--registry", type=pathlib.Path,
                        default=DEFAULT_REGISTRY)
    parser.add_argument("--output-dir", type=pathlib.Path,
                        default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--manifest", type=pathlib.Path,
                        default=DEFAULT_MANIFEST)
    parser.add_argument("--filter", help="emit only names containing this text")
    parser.add_argument("--start", type=int, default=0,
                        help="zero-based first filtered signature")
    parser.add_argument("--count", type=int,
                        help="number of filtered signatures to emit")
    parser.add_argument("--check", action="store_true",
                        help="fail if the output is not up to date")
    args = parser.parse_args()
    signatures, excluded, inventory = density_signatures(
        args.stanc, args.registry)
    partial = bool(args.filter) or args.start != 0 or args.count is not None
    if partial:
        if args.output_dir.resolve() == DEFAULT_OUTPUT_DIR.resolve() or \
                args.manifest.resolve() == DEFAULT_MANIFEST.resolve():
            parser.error("partial generation requires both a noncanonical "
                         "--output-dir and --manifest")
    if args.filter:
        signatures = [signature for signature in signatures
                      if args.filter in signature.name]
    signatures = signatures[args.start:None if args.count is None
                             else args.start + args.count]
    # Every overload once with all its real arguments parameter-dependent,
    # then, when it has at least two such arguments, once per argument
    # with only that one parameter-dependent: the mixed data/parameter
    # instantiations the all-active call never reaches. An overload with
    # fewer than two would only repeat its all-active instantiation.
    rendered = [
        (signature.canonical_id + ("" if position is None else f"@{position}"),
         functools.partial(render_case, signature, position))
        for signature in signatures
        for positions in [real_argument_positions(signature)]
        for position in [None] + (positions if len(positions) >= 2 else [])
    ]
    groups = partitions(rendered, CASES_PER_MODEL)
    expected = {
        args.output_dir / f"density_signatures_{index:02d}.stan":
            render_model(index, len(groups), group)
        for index, group in enumerate(groups, 1)
    }
    models = [generated_model_record(path, source,
                                     [identity for identity, _ in group], ROOT)
              for (path, source), group in zip(expected.items(), groups)]
    registered_names = registry_by_name(args.registry, "density")
    dumped_names = {signature.name for signature in inventory.signatures}
    tested_names = {signature.name for signature in signatures}
    names_with_excluded_signatures = {
        item["signature"].split("(", 1)[0] for item in excluded
    }
    explicitly_excluded_names = names_with_excluded_signatures - tested_names
    partially_excluded_names = names_with_excluded_signatures & tested_names
    missing_names = set(registered_names) - dumped_names
    unaccounted_names = (set(registered_names) - tested_names -
                         explicitly_excluded_names - missing_names)
    if unaccounted_names and not partial:
        raise RuntimeError(
            "registered probability functions have no generated coverage or "
            "explicit exclusion: " + ", ".join(sorted(unaccounted_names)))
    manifest = {
        "generator": "tools/generate_density_signature_model.py",
        "partial": partial,
        "stanc_build_id": portable_build_id(inventory.stanc_build_id),
        "signature_dump_sha256": inventory.raw_sha256,
        "registry_name_count": len(registered_names),
        "tested_registry_name_count": len(tested_names),
        "tested_registry_names": sorted(tested_names),
        "explicitly_excluded_registry_names": sorted(
            explicitly_excluded_names),
        "partially_excluded_registry_names": sorted(
            partially_excluded_names),
        "unaccounted_registry_names": sorted(unaccounted_names),
        "tested_signature_count": len(signatures),
        "tested_case_count": len(rendered),
        "excluded_signature_count": len(excluded),
        "missing_from_stanc": sorted(set(registered_names) - dumped_names),
        "excluded": by_reason(excluded),
        "models": models,
    }
    okay = write_generated_outputs(expected, args.output_dir, FILE_GLOB,
                                   args.manifest, manifest, args.check, ROOT)
    if not args.check:
        print(f"generated {len(groups)} models covering {len(rendered)} "
              "density cases")
    return 0 if okay else 1


if __name__ == "__main__":
    raise SystemExit(main())
