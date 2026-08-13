"""Semantic value profiles for generated Stan calls.

Types cannot tell the generator that `log_diff_exp(a, b)` needs a > b or
that the first argument of `log_mix` is a probability.  Profiles encode only
those mathematical domains.  They never encode whether stanli supports the
function; a reference-invalid profile remains a generator gap.
"""

from __future__ import annotations

import dataclasses
from typing import Optional, Tuple

from .signatures import AtomicType, Signature


DEFAULT_REAL_CENTERS = (0.5, 1.25, 0.75, 1.5, 2.0, 0.9, 1.8, 0.3)
DEFAULT_INT_CENTERS = (2, 3, 1, 4, 2, 5, 3, 1)


@dataclasses.dataclass(frozen=True)
class DomainProfile:
    id: str
    names: Tuple[str, ...]
    real_centers: Tuple[float, ...] = ()
    int_centers: Tuple[int, ...] = ()
    perturbation: float = 0.0625
    # Some internal Stan Math signatures impose a data-only qualifier which
    # older stanc dumps omit at the outer level.  Rows are (arity, positions),
    # with zero-based positions.  This affects differentiation, not support.
    data_positions_by_arity: Tuple[Tuple[int, Tuple[int, ...]], ...] = ()
    centers_by_types: Tuple[
        Tuple[Tuple[str, ...], Tuple[object, ...]], ...
    ] = ()

    def matches(self, signature: Signature) -> bool:
        return signature.name in self.names


PROFILES = (
    DomainProfile("unit-interval", ("acos", "asin", "atanh", "log1m",
                                     "logit"), (0.35,)),
    DomainProfile("positive", ("acosh", "log", "log2", "log10", "sqrt",
                                "inv_sqrt", "lgamma", "trigamma",
                                "digamma"), (1.4,)),
    DomainProfile(
        "ordered-pair", ("log_diff_exp",),
        centers_by_types=(
            (("int", "real"), (2, 0.5)),
            (("real", "int"), (2.5, 2)),
            (("real", "real"), (1.5, 0.5)),
        ),
    ),
    DomainProfile("log-mix", ("log_mix",), (0.35, -0.2, 0.7)),
    DomainProfile("falling-factorial",
                  ("falling_factorial", "log_falling_factorial"),
                  (3.5, 1.2)),
    DomainProfile("rising-factorial",
                  ("rising_factorial", "log_rising_factorial"),
                  (1.5, 1.2)),
    DomainProfile("beta-family", ("beta", "lbeta", "inc_beta",
                                   "inc_beta_ddz"), (2.0, 3.0, 0.4)),
    DomainProfile("gamma-family", ("gamma_p", "gamma_q"), (2.0, 1.3)),
    DomainProfile(
        "piecewise-pair", ("fdim", "fmax", "fmin"),
        centers_by_types=(
            (("int", "real"), (2, 2.0)),
            (("real", "int"), (2.0, 2)),
            (("real", "real"), (1.0, 1.0)),
        ),
    ),
    DomainProfile(
        "remainder", ("fmod",),
        centers_by_types=((('real', 'real'), (2.5, 1.0)),),
    ),
    DomainProfile(
        "hypergeometric-1f0", ("hypergeometric_1F0",),
        centers_by_types=((('real', 'real'), (0.5, 0.25)),),
    ),
    DomainProfile(
        "hypergeometric-2f1", ("hypergeometric_2F1",),
        centers_by_types=(
            (("real", "real", "real", "real"), (0.5, 0.75, 1.5, 0.25)),
        ),
    ),
    DomainProfile("lambert-wm1", ("lambert_wm1",), (-0.2,)),
    DomainProfile("log-one-minus-exp", ("log1m_exp",), (-0.5,)),
    DomainProfile("multivariate-log-gamma", ("lmgamma",),
                  centers_by_types=((('int', 'real'), (2, 2.0)),)),
    DomainProfile(
        "log-ordered-pair", ("log_inv_logit_diff",),
        centers_by_types=(
            (("int", "real"), (2, 0.5)),
            (("real", "int"), (2.5, 2)),
            (("real", "real"), (1.0, -0.5)),
        ),
    ),
    DomainProfile(
        "choose", ("lchoose",),
        centers_by_types=(
            (("int", "real"), (4, 2.0)),
            (("real", "int"), (4.0, 2)),
            (("real", "real"), (4.0, 2.0)),
        ),
    ),
    DomainProfile("lower-unconstrain", ("lower_bound_unconstrain",),
                  centers_by_types=((('real', 'real'), (1.5, 0.5)),)),
    DomainProfile(
        "lower-upper-bounds",
        ("lower_upper_bound_constrain", "lower_upper_bound_jacobian",
         "lower_upper_bound_unconstrain"),
        centers_by_types=(
            (("real", "real", "real"), (0.0, -1.0, 1.0)),
        ),
    ),
    DomainProfile("log-probability", ("std_normal_log_qf",), (-0.5,)),
    DomainProfile("bessel", ("bessel_first_kind", "bessel_second_kind",
                              "modified_bessel_first_kind",
                              "modified_bessel_second_kind"),
                  (1.4,), (2,)),
    DomainProfile(
        "wiener-unnormalized",
        ("wiener_lccdf_unnorm", "wiener_lcdf_unnorm"),
        (1.2, 0.45, 1.1, 0.35, 0.4, 0.2, 0.1, 0.1, 1e-8),
        data_positions_by_arity=((6, (5,)), (9, (8,))),
    ),
)


NONDIFFERENTIABLE_NAMES = frozenset({"ceil", "floor", "round", "step",
                                     "trunc"})


# Outcome first, followed by distribution parameters.  These values are
# deliberately away from support boundaries and symmetry points so each real
# argument normally contributes a nonzero partial.  One profile is shared by
# the density, CDF, log-CDF, and log-CCDF family; integer outcomes remain data.
PROBABILITY_CENTERS = {
    "bernoulli": (0, 0.4),
    "bernoulli_logit": (0, 0.4),
    "beta": (0.4, 2.0, 1.3),
    "beta_binomial": (3, 10, 2.0, 3.0),
    "beta_neg_binomial": (3, 2.0, 3.0, 1.3),
    "beta_proportion": (0.4, 0.6, 2.0),
    "binomial": (3, 10, 0.4),
    "binomial_logit": (3, 10, 0.4),
    "cauchy": (0.4, 0.2, 1.3),
    "chi_square": (1.4, 3.0),
    "double_exponential": (0.4, 0.2, 1.3),
    "exp_mod_normal": (0.4, 0.2, 1.3, 0.7),
    "exponential": (1.4, 0.7),
    "frechet": (1.4, 2.0, 1.3),
    "gamma": (1.4, 2.0, 1.3),
    "gumbel": (0.4, 0.2, 1.3),
    "inv_chi_square": (1.4, 3.0),
    "inv_gamma": (1.4, 2.0, 1.3),
    "logistic": (0.4, 0.2, 1.3),
    "loglogistic": (1.4, 2.0, 1.3),
    "lognormal": (1.4, 0.2, 1.3),
    "neg_binomial": (3, 2.0, 1.3),
    "neg_binomial_2": (3, 2.0, 1.3),
    "neg_binomial_2_log": (3, 0.4, 1.3),
    "normal": (0.4, 0.2, 1.3),
    "pareto": (2.4, 1.0, 1.3),
    "pareto_type_2": (1.4, 0.0, 1.2, 1.3),
    "poisson": (3, 1.3),
    "poisson_log": (3, 0.4),
    "rayleigh": (1.4, 1.3),
    "scaled_inv_chi_square": (1.4, 3.0, 1.2),
    "skew_double_exponential": (0.4, 0.2, 1.3, 0.6),
    "skew_normal": (0.4, 0.2, 1.3, 0.7),
    "std_normal": (0.4,),
    "student_t": (0.4, 3.0, 0.2, 1.3),
    "uniform": (0.4, -1.0, 1.5),
    "von_mises": (0.4, 0.2, 1.3),
    "weibull": (1.4, 2.0, 1.3),
    "yule_simon": (3, 1.3),
}


def _probability_family(name: str) -> Optional[str]:
    for suffix in ("_lpdf", "_lpmf", "_lccdf", "_lcdf", "_cdf"):
        if name.endswith(suffix):
            return name[:-len(suffix)]
    return None


def probability_values(signature: Signature) \
        -> Optional[Tuple[Tuple[object, ...], str, float, Tuple[int, ...],
                          Tuple[int, ...]]]:
    """Support-aware values for all scalar probability families we know.

    The final tuple lists parameter-lane indices whose mathematical partial is
    identically zero at every interior point.  They still originate in theta
    and are compared; they are only exempt from the nonzero-information check.
    """
    family = _probability_family(signature.name)
    if family is None:
        return None
    if family == "wiener":
        if not 5 <= signature.arity <= 9:
            return None
        # y, boundary separation, nondecision time, bias, drift, followed by
        # inter-trial variability and numerical precision parameters.
        values = (1.5, 1.2, 0.2, 0.45, 0.1, 0.1, 0.1, 0.05, 1e-5)
        if signature.arity == 7:
            # The seven-argument overload is the six-parameter density plus a
            # data-only derivative precision, not the first seven parameters
            # of the eight-parameter density.
            centers = values[:6] + (1e-5,)
            data_positions = (6,)
        else:
            centers = values[:signature.arity]
            data_positions = (8,) if signature.arity == 9 else ()
        return (centers, "probability:wiener", 0.03125,
                data_positions, ())
    centers = PROBABILITY_CENTERS.get(family)
    if centers is None or len(centers) != signature.arity:
        return None
    types = tuple(argument.name if isinstance(argument, AtomicType) else None
                  for argument in signature.arguments)
    if any((kind == "int") != isinstance(value, int)
           for kind, value in zip(types, centers)):
        # A small number of Math overloads accept an integer where the usual
        # distribution parameter is real.  Preserve the semantic value while
        # spelling it as the type stanc requested.
        centers = tuple(int(value) if kind == "int" else float(value)
                        for kind, value in zip(types, centers))
    zero_lanes = (0,) if family == "uniform" else ()
    return (tuple(centers), f"probability:{family}", 0.0625, (), zero_lanes)


def semantic_inapplicability(signature: Signature) -> Optional[str]:
    """Named mathematical semantics which types alone cannot reveal."""
    if signature.name in NONDIFFERENTIABLE_NAMES:
        return "The function is piecewise constant with a zero Stan gradient."
    return None


def profile_for(signature: Signature) -> Optional[DomainProfile]:
    matches = [profile for profile in PROFILES if profile.matches(signature)]
    if len(matches) > 1:
        raise ValueError(f"{signature.canonical_id}: overlapping domain profiles")
    return matches[0] if matches else None


def scalar_values(signature: Signature) \
        -> Tuple[Tuple[object, ...], str, float, Tuple[int, ...],
                 Tuple[int, ...]]:
    """One deterministic center per scalar argument plus profile metadata."""
    probability = probability_values(signature)
    if probability is not None:
        return probability
    profile = profile_for(signature)
    types = tuple(argument.name if isinstance(argument, AtomicType) else ""
                  for argument in signature.arguments)
    exact_centers = dict(profile.centers_by_types).get(types) \
        if profile is not None else None
    if exact_centers is not None:
        data_positions = dict(profile.data_positions_by_arity).get(
            signature.arity, ())
        return (tuple(exact_centers), profile.id, profile.perturbation,
                data_positions, ())
    reals = profile.real_centers if profile and profile.real_centers \
        else DEFAULT_REAL_CENTERS
    integers = profile.int_centers if profile and profile.int_centers \
        else DEFAULT_INT_CENTERS
    real_index = 0
    int_index = 0
    values = []
    for argument in signature.arguments:
        if not isinstance(argument, AtomicType):
            raise ValueError(f"{signature.canonical_id}: not a scalar argument")
        if argument.name == "real":
            values.append(reals[real_index % len(reals)])
            real_index += 1
        elif argument.name == "int":
            values.append(integers[int_index % len(integers)])
            int_index += 1
        else:
            raise ValueError(
                f"{signature.canonical_id}: unsupported scalar {argument.name}")
    data_positions = ()
    if profile is not None:
        data_positions = dict(profile.data_positions_by_arity).get(
            signature.arity, ())
    return (tuple(values), profile.id if profile else "generic-scalar",
            profile.perturbation if profile else 0.0625, data_positions, ())
