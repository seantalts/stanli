#!/usr/bin/env python3
"""Differential corpus verification against committed CmdStan references.

tools/verify_sample.py runs CmdStan itself and records the exact lp and
gradient values it printed into docs/internal/artifacts/corpus-refs.json.gz. This script
replays stanli against those recorded values, which needs no CmdStan, no
C++ toolchain, and no 2 GB checkout: just a stanli_check binary and the
posteriordb model + data files. That is what lets the strongest oracle in
the project run in CI on every push, on every platform.

The primary references were generated on macOS arm64 with Apple's libm.
Other platforms' libm implementations round transcendentals differently,
so the cross-platform gate is deliberately looser than the 1e-10 the
generating rig holds itself to: 1e-9 relative. Same-platform recordings
also enforce the ULP limits below. Every bug class that has actually reached the
corpus (silent in-place corruption at 1.7e+05 relative, quadratic
recompute, dropped tape links) sits many orders of magnitude above it,
and honest cross-libm drift sits well below.

Every model is evaluated at all three deterministic points, and every
point carries its own recorded reference; see POINTS below.

Usage: tools/verify_refs.py PDB_DIR [--check BIN] [--max-rel X]
                            [--jobs N] [--timeout S] [model ...]
       tools/verify_refs.py PDB_DIR --wa-report [--filter SUBSTR]
       tools/verify_refs.py PDB_DIR --wa-headers CMDSTAN_DIR [model ...]
Exit nonzero if any referenced model fails to run at any point, changes
shape, or exceeds the gate.

       tools/verify_refs.py --corpus NAME [--model M ...] [--fixture-passes]
replays one of the local corpora in CORPORA, as CTest does.
--fixture-passes compiles with the O1 passes of the MIR fixtures
(tools/gen_fixtures.py O1_PASSES), which add function inlining and
partial evaluation to the shipping passes.
"""
from __future__ import annotations

import argparse
import collections
import concurrent.futures
import dataclasses
import gzip
import json
import math
import os
import pathlib
import platform
import re
import struct
import subprocess
import sys
import tempfile
from fractions import Fraction

from corpus_inventory import (CorpusCase, corpus_cases, local_cases, materialize_data,
                              source_digest)
import toolchain
import check_status
import gen_fixtures
import importlib.util


def _hp_metrics():
    """tools/hp/metrics.py, which measures the distance from an 80-digit
    value for both this gate and tools/hp/ledger_accuracy.py. It uses only
    the standard library. It is loaded from its path, because putting
    tools/hp on sys.path would let its common.py shadow other modules."""
    spec = importlib.util.spec_from_file_location(
        "hp_metrics", pathlib.Path(__file__).resolve().parent / "hp" / "metrics.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


hp_metrics = _hp_metrics()

REPO = pathlib.Path(__file__).resolve().parent.parent
N_SAMPLER_COLS = 7
REFS_PATH = REPO / "docs" / "internal" / "artifacts" / "corpus-refs.json.gz"
# The reference file's format. Bumping this is a hard break on purpose:
# load_refs refuses anything else rather than reading what it recognizes
# and silently skipping the rest, which for an oracle is the failure mode
# that matters. Schema 1 held one point per model; see load_refs.
SCHEMA = 2
# The deterministic unconstrained points stanli_check and ref_driver both
# know (eval_point, defined identically in each). Every model carries a
# recorded CmdStan reference at every one of them.
#
# It did not always: schema 1 recorded exactly one point per model,
# because the recorder walked the list and stopped at the first point
# both engines accept and put the model inside its support. That search
# actively selects AWAY from a point that crashes -- which is how
# reductions_allowed segfaulted at point 2 for a month while the corpus
# ran green at point 0, and 25 of the 120 corpus models carry a
# conditional whose other side only some point reaches. Evaluating the
# other two points closed the crash gap; recording them closes the rest,
# since a wrong-but-finite gradient at an unreferenced point has nothing
# to be wrong against. What a point with no reference is still held to,
# for the cases where recording is impossible, is in probe_point.
POINTS = (0, 1, 2)

# (model, point) pairs whose recorded reference is not enforced, because
# stanli is known to disagree with it and the bug is open. The reference
# itself is recorded from CmdStan regardless -- references describe
# CmdStan, not stanli -- so deleting an entry here is all it takes for the
# full three-point parity to bite once the fix lands.
#
# Nothing may be added here on an argument. Every entry names the live
# CmdStan run that settled it: tools/ref_driver.cpp compiled for that
# model and run at that point. Empty since the pow-at-zero derivative that
# held s2_ar_cov's two points was fixed.
QUARANTINED = {}

WRITE_ARRAY = "write_array"
WHOLE_MODEL = "the whole model"


@dataclasses.dataclass(frozen=True)
class Gap:
    """What a KNOWN_GAPS entry excuses, and why.

    WRITE_ARRAY excuses a write_array section that runs through the
    interpreter or fails; lp and the gradient are still compared at every
    point. WHOLE_MODEL excuses any failure short of a crash.
    """
    excuses: str
    reason: str

    def __post_init__(self):
        if self.excuses not in (WRITE_ARRAY, WHOLE_MODEL):
            raise ValueError(f"a gap excuses {WRITE_ARRAY!r} or "
                             f"{WHOLE_MODEL!r}, not {self.excuses!r}")


# Models stanli cannot run in full, mapped to what is excused. The
# references are recorded from CmdStan the same way every other model's
# are, so the only thing an entry suppresses is the failure; when the gap
# closes, the model matches its references, the replay reports GAP_CLOSED,
# and the run stays red until the entry is deleted. A crash is never
# excused: a segfault and a refusal are different bugs.
KNOWN_GAPS = {}

# Corpus models that run through the MIR interpreter, mapped to why. Any
# other model that reports an interpreted log density or write_array
# fails with INTERPRETER, and so does a listed model that no longer
# does, as INTERPRETED does for the local corpora.
CORPUS_INTERPRETED = {}

# (model, point) pairs excused from probe_point's finite-gradient rule,
# for points that carry no reference at all. Empty while every point is
# referenced -- pair_dev already counts NaN against NaN as agreement, so
# kronecker_gp at point 2 (435 of its 438 gradients nonfinite on BOTH
# engines, at the identical lp -187.85795069042379, because the all-zeros
# point makes its covariance degenerate outright) needs no entry: its
# reference records those nonfinite values and stanli reproduces them.
NONFINITE_GRAD_OK = {}

# Models whose gradient entries are held to ILL_CONDITIONED_GATE instead of
# the clean gate, off the platforms whose recordings they match. lp and
# write_array keep the clean gate. Every entry factors an
# exponentiated-quadratic covariance that brms holds up with a 1e-12 jitter
# on the diagonal, and the smallest Cholesky pivot at the evaluation points
# is 3.7e-12 for i320_gp_expquad, 1.1e-12 for sw_gp and 1.8e-12 for
# s2_gp_by_gr against a diagonal of 1, so the factorization is singular to
# machine precision. Moving the GP covariates by one ulp on arm64 moves the
# latent-GP gradients at point 2 by 1.9e-7 and 4.7e-8 relative. stanli
# matches the arm64 recording to 4.4e-16 on arm64, and the Linux x86-64
# recording within the 10-ULP brms gate there.
ILL_CONDITIONED = {
    "i320_gp_expquad": "cholesky_decompose of a jittered exp-quad "
                       "covariance whose smallest pivot is 3.7e-12",
    "s2_gp_by_gr": "cholesky_decompose of five jittered exp-quad "
                   "covariances whose smallest pivot is 1.8e-12",
    "sw_gp": "cholesky_decompose of a jittered exp-quad covariance whose "
             "smallest pivot is 1.1e-12",
    "examplemodels_gp_fit_multi_output":
        "cholesky_decompose of an exp-quad covariance of 101 points with a "
        "1e-9 jitter, condition number 5.6e10 at point 0",
}
# About 4x the largest gradient deviation measured against the arm64
# recording on another platform: 3.58e-7 for s2_gp_by_gr on macOS x86-64
# (main run 44727913083), and 4.15e-7 for it before the CmdStan 2.40
# refresh (tests/brms/README.md). The others: sw_gp 1.72e-7 on Linux
# aarch64, i320_gp_expquad 1.03e-7 on Linux x86-64 before that platform
# had its own recording.
# The Linux x86-64 CmdStan recording is within 5.7e-11 of the arm64 one on
# lp and 1.6e-16 on write_array for all three models.
ILL_CONDITIONED_GATE = 1.5e-6
# ILL_CONDITIONED models whose log density is held to its own gate as well,
# because the likelihood reads the ill-conditioned factor. For
# examplemodels_gp_fit_multi_output, 89 of the covariance's 101 eigenvalues
# sit at the jitter. At point 0, against an 80-digit evaluation, the macOS
# arm64 log density (stanli and CmdStan alike) is 7.3e-9 off and grad[1]
# 6.6e-8 off; Linux x86-64 measured grad[1] 3.9e-7 from the arm64 recording.
# Moving log(rho) by up to 2.4e-15 on arm64 moves the log density by up to
# 1.1e-8 and the gradient by up to 6.2e-7 (point 1) from the recording; Stan
# Math's true change over that step is below 1e-14. The lp gate is about 4x
# the largest of these.
ILL_CONDITIONED_LP_GATE = {"examplemodels_gp_fit_multi_output": 5e-8}

@dataclasses.dataclass(frozen=True)
class Mismatch:
    """The lp/gradient value indices of a MISMATCH model that disagree with
    CmdStan, why, and the smallest gate they get (see gate_for)."""
    indices: tuple
    reason: str
    floor: float = 0.0


# For each model with a point recorded MISMATCH, the values that disagree
# with CmdStan. Only these values get the MISMATCH gate of gate_for; the
# rest of the point keeps the clean gate.
MISMATCH_OUTPUTS = {
    # The floor is 4x the largest deviation measured on another platform,
    # 8.2e-3 at point 2 on x86_64 (CI run 32637919029), because the
    # amplification is not proportional to the deviation that arm64
    # recorded (1.05e-3 at that point).
    "kronecker_gp": Mismatch(
        (1, 2), "the two gradient entries that flow through eigenvectors_sym "
                "of a covariance with eigenvalue gaps down to 6.5e-17",
        floor=4.0 * 8.2e-3),
    "stanguide_ode_adjoint": Mismatch(
        (1, 2, 5), "the gradient entries of y0 and theta come from the "
                   "adjoint ODE solve at the guide's tolerances of 1e-6; "
                   "solved at 1e-11 the two engines agree to 1e-10, and "
                   "at 1e-6 both are up to 4.4e-5 from that solution "
                   "(NOTES in tools/corpus.py)"),
}


@dataclasses.dataclass(frozen=True)
class Floored:
    """lp/gradient value indices held to their own scaled-error gate."""
    indices: frozenset
    gate: float
    reason: str
    # The gate of index 0, the log density, where it differs from `gate`.
    lp_gate: float = None

    def gate_of(self, index):
        return self.lp_gate if index == 0 and self.lp_gate else self.gate


def floored_outputs(model, pt, default):
    """The Floored values of one recorded point, or None."""
    if "values" not in pt:
        return None
    if model in ILL_CONDITIONED:
        lp_gate = ILL_CONDITIONED_LP_GATE.get(model)
        return Floored(frozenset(range(0 if lp_gate else 1, len(pt["values"]))),
                       gate_for(model, pt, default), ILL_CONDITIONED[model],
                       lp_gate and max(lp_gate, default))
    if pt.get("status") == "MISMATCH":
        mismatch = MISMATCH_OUTPUTS[model]
        return Floored(frozenset(mismatch.indices), gate_for(model, pt, default),
                       mismatch.reason)
    return None


# These cancellation regressions used to pass the scaled-error gate despite
# hundreds or thousands of ULP of error. Reuse the same three-point corpus
# replay to enforce the tighter contract against a same-platform CmdStan
# recording, without another model/CI sweep. Cross-platform libm differences
# are still held to the scaled-error gate above.
ULP_LIMITS = {source.stem: 10 for source in (REPO / "tests" / "brms").glob("*.stan")}
# Raised limits, each measured against the 80-digit evaluator in tools/hp.
# The truncated Poisson's grad[4] sums its terms pairwise: at point 0 stanli
# is 0.0 and CmdStan 16.0 scaled ULP from the true value.
ULP_LIMITS.update(dict.fromkeys(("i320_pois_trunc_both", "i320_pois_trunc_ub"),
                                16))
# b_x_Intercept = Intercept - dot_product(means_X, b) takes the column mean of
# 40 values, which stanli adds with compensation: at point 0 stanli is 3.9
# and CmdStan 67.9 ULP from the 80-digit value, and stanli is 64 ULP from
# CmdStan.
ULP_LIMITS.update(dict.fromkeys(("s2_mi_lognormal", "s2_mi_trunc_lb"), 64))
# Gradient ULP is measured against the largest gradient entry for these
# fixtures; lp and write_array outputs keep per-element ULP.
_REGROUPED_ADJOINT = ("adjoint sums regrouped by vectorized random-effect "
                      "loops; checked against a 60-digit reference")
# brms centres each predictor column on its mean in transformed data. The
# gradient entry of a centred predictor's coefficient is a sum over rows of
# the centred column times a residual, which cancels. Stanli's compensated
# mean gives centred data closer to the exact centring than CmdStan's (whose
# mean and centred column are those of stanli without compensation, checked
# with CmdStan on sw_negbinomial), and the entries then move by several to
# hundreds of their own ULP either way, as each platform's own rounding of
# the cancelling sum also does.
#
# The rule: a brms fixture whose transformed data centres a predictor on a
# mean of more than 32 rows (which stanli adds with compensation) measures its
# gradient against the largest entry when, at point 0 or 1, the entry of a
# centred coefficient cancels so far that one rounding per term is at least 5
# ULP of the entry, half the per-element limit. The 50-digit interpreter
# measures it from d lp / d Xc[i, j] = b[j] * s[i] (point 2 has b = 0 and is
# not measured): of the 105 fixtures that centre, 79 have this property, with
# terms that add up to 6.7 to 17,951 times the entry and one rounding per
# term 5.2 to 12,904 ULP of it. Ten of them were already measured this way
# for their regrouped adjoints, and s2_wiener (below) cannot be measured.
# The others that centre stay per element: 21 cancel less, s2_invgaussian
# has no 50-digit measure, and three centre fewer than 33 rows, where the
# mean is unchanged.
#
# sw_mv_rescor and sw_student show why the rule cannot be "failed on macOS":
# both stayed within 10 ULP there, and on Linux x86-64 grad[4] at point 1 and
# grad[1] at point 0 (terms 10.6 and 21.8 times the entry, 8.9 and 15.1 ULP
# per rounding) were 14 and 16 ULP from CmdStan, while stanli was 4.9 and 9.1
# and CmdStan 9.1 and 6.9 ULP from the 80-digit value, and both were within
# 0.6 ULP of it against the largest entry.
#
# Against the largest entry, stanli is at most 7 ULP from CmdStan on macOS.
# Where the 80-digit gradient decides (64 of these fixtures), stanli is
# closer to it than CmdStan in 21, farther in 6 (by at most 3.5 ULP, in
# s2_fcor) and the same in 37. In four censored or truncated fixtures
# (s2_cens_interval, s2_weights_trunc, sw_cens, sw_trunc) both engines are
# 1e9 ULP or more from it: Stan Math's normal_lcdf and normal_lccdf form their
# partials from Abramowitz and Stegun's approximation 7.1.26. i319_pois_re2
# has only directional 80-digit derivatives.
_CENTRED_GRADIENT = ("gradient entries of mean-centred predictors cancel; "
                     "checked against an 80-digit reference")
GRADIENT_AGAINST_LARGEST = {
    name: _REGROUPED_ADJOINT for name in (
        "i320_pois_trunc_both", "i320_pois_trunc_ub", "s2_car",
        "s2_car_esicar", "s2_gr_by", "s2_me2_nomecor", "s2_mm",
        "s2_mm_weights", "s2_mmc", "s2_multinomial", "sw_gamma",
        "sw_re_bern", "sw_skewnormal", "sw_weibull")
}
GRADIENT_AGAINST_LARGEST.update({
    name: _CENTRED_GRADIENT for name in (
        "i319_gauss_re", "i319_negbin_fixed", "i319_negbin_re",
        "i319_pois_fixed", "i319_pois_re", "i319_pois_re2", "i320_sratio_cs",
        "i320_sratio_plain", "s2_ar_cov", "s2_car_icar", "s2_categorical_re",
        "s2_cens_interval", "s2_cosy", "s2_cox", "s2_cumulative_cauchit",
        "s2_custom_vreal", "s2_discrete_weibull", "s2_dist_sigma_re",
        "s2_fcor", "s2_gev", "s2_hurdle_cumulative", "s2_index_mi",
        "s2_mixture_theta", "s2_mv_shared_re", "s2_rate", "s2_sar",
        "s2_sar_error", "s2_unstr", "s2_weights_trunc", "s2_zi_beta",
        "s2_zoi_beta", "sw_acat", "sw_acat_cs", "sw_ar", "sw_arma",
        "sw_bernoulli", "sw_binomial", "sw_categorical", "sw_cratio",
        "sw_cratio_cs", "sw_cumulative", "sw_cumulative_cs", "sw_dist_sigma",
        "sw_exgaussian", "sw_gaussian", "sw_hurdle_gamma",
        "sw_hurdle_lognormal", "sw_hurdle_pois", "sw_lognormal", "sw_ma",
        "sw_mi", "sw_mixture", "sw_mv_norescor", "sw_mv_rescor",
        "sw_negbinomial", "sw_poisson", "sw_re_gauss", "sw_re_negbin",
        "sw_re_pois", "sw_re_slope", "sw_sratio", "sw_student", "sw_trunc",
        "sw_vonmises", "sw_weights", "sw_zi_binomial", "sw_zi_negbin",
        "sw_zi_poisson")
})
# The same centring, in two models that the 80-digit reference cannot judge.
# s2_wiener: tools/hp has no wiener density, so only the part that changed is
# measured: stanli's column mean is 0.10 ULP from the exact mean and CmdStan's
# 1.90, and stanli's centred column is at most 0.55 ULP from the exact
# centring, CmdStan's 1.90. The gradient is not checked against a true
# value; against its largest entry stanli is 1 ULP from CmdStan.
# sw_cens: Stan Math's normal_lcdf and normal_lccdf form their partials from
# Abramowitz and Stegun's approximation 7.1.26, so both engines' gradients
# are 2e-7 to 6e-7 relative from the true derivative of their own lp, which
# itself agrees with the 80-digit value to 2e-16. Against its largest entry
# stanli is 1 ULP from CmdStan.
GRADIENT_AGAINST_LARGEST["s2_wiener"] = (
    "gradient entries of mean-centred predictors cancel; no 80-digit "
    "reference for wiener, the centred data is checked instead")
GRADIENT_AGAINST_LARGEST["sw_cens"] = (
    "gradient entries of mean-centred predictors cancel; Stan Math's "
    "censored-normal partials are approximations to 2e-7, so the 80-digit "
    "reference cannot decide")
PLATFORM_REFS = {
    "Linux x86_64": REPO / "docs" / "internal" / "artifacts" / "corpus-refs-linux-x86_64.json.gz",
}
# Sparse platform recordings address documented cross-platform instability
# without changing the original point gates or output coverage. Select solely
# by the tested platform/compiler, before seeing any candidate values.
SPARSE_PLATFORM_REFS = {
    "Darwin x86_64": (REPO / "docs" / "internal" / "artifacts" / "corpus-refs-darwin-x86_64.json.gz",
                      "clang", frozenset({"kronecker_gp"})),
}
COMPILER_REFS = {
    ("Linux x86_64", "gcc"): REPO / "docs" / "internal" / "artifacts" / "corpus-refs-linux-x86_64-gcc.json.gz",
}


def load_refs(path=REFS_PATH):
    """(models, provenance) from the reference file, or a hard failure.

    The one reader of docs/internal/artifacts/corpus-refs.json.gz, shared by the replay, the
    recorder, the lite-build cross-check and the corpus scoreboard, so a
    schema change cannot land in one and not the others. `models` maps a
    model name to {"data", "primary", "points": {"0": {...}, ...}};
    `provenance` is the default recording rig. An entry's `recorded` block
    overrides it and preserves independently imported recording metadata.
    """
    blob = json.loads(gzip.decompress(path.read_bytes()))
    schema = blob.get("schema")
    if schema != SCHEMA:
        raise SystemExit(
            f"{path} is schema {schema!r}, and these tools read schema "
            f"{SCHEMA}. Schema 1 recorded one point per model and its "
            f"entries are not a subset of this format, so half-reading it "
            f"would gate on a fraction of the corpus while reporting a "
            f"full pass. Re-record with tools/verify_sample.py "
            f"--from-refs, or check out the commit that matches the file.")
    return blob["models"], blob["recorded"]


def native_platform():
    return f"{platform.system()} {platform.machine()}"


def replay_refs(target_platform, compiler=None):
    """Select independently recorded answers for the runtime's platform.

    The primary recording remains the cross-platform fallback. A platform
    supplement covers every ULP-gated fixture; compiler supplements replace
    the few answers where independent CmdStan builds differ. All use the
    same dependency pins and unchanged ULP limits. Sparse platform supplements
    preserve the original gates and output coverage for their declared models.
    Select before evaluation,
    never by proximity to the candidate. Keep this out of load_refs, which
    also serves the recorder.
    """
    refs, recorded = load_refs()
    refs = {name: {**ref, "recorded": ref.get("recorded", recorded)}
            for name, ref in refs.items()}
    sources = []
    if target_platform in PLATFORM_REFS:
        sources.append((PLATFORM_REFS[target_platform], True, None))
    if (target_platform, compiler) in COMPILER_REFS:
        sources.append((COMPILER_REFS[target_platform, compiler], False, None))
    if target_platform in SPARSE_PLATFORM_REFS:
        path, expected_compiler, models = SPARSE_PLATFORM_REFS[target_platform]
        if compiler != expected_compiler:
            raise SystemExit("sparse platform reference compiler mismatch")
        sources.append((path, False, models))
    for path, complete, sparse_models in sources:
        extra, rig = load_refs(path)
        if complete and set(extra) != set(ULP_LIMITS):
            raise SystemExit("platform references must cover every ULP-gated fixture")
        if sparse_models is not None:
            if set(extra) != sparse_models or not set(extra) <= set(refs):
                raise SystemExit("sparse platform references must cover their declared fixtures")
        elif not extra or not set(extra) <= set(ULP_LIMITS):
            raise SystemExit("compiler references must contain ULP-gated fixtures")
        for name, ref in extra.items():
            provenance = ref.get("recorded", rig)
            if provenance.get("platform") != target_platform:
                raise SystemExit(f"{name}: platform reference provenance mismatch")
            if not complete and provenance.get("compiler_family") != compiler:
                raise SystemExit(f"{name}: reference compiler mismatch")
            for pin in ("cmdstan", "stan", "math", "stanc3"):
                if provenance.get(pin) != refs[name]["recorded"].get(pin):
                    raise SystemExit(f"{name}: platform reference {pin} pin mismatch")
            if not all(ref.get(key) for key in ("source_sha256", "data_sha256")):
                raise SystemExit(f"{name}: platform reference input hashes are required")
            if set(ref.get("points", {})) != {str(p) for p in POINTS}:
                raise SystemExit(f"{name}: platform reference points are incomplete")
            if sparse_models is not None:
                original = refs[name]
                for key in ("data", "primary", "strict"):
                    if ref.get(key) != original.get(key):
                        raise SystemExit(f"{name}: sparse reference {key} metadata changed")
                for point, pt in ref["points"].items():
                    old = original["points"][point]
                    for key in ("status", "max_rel"):
                        if pt.get(key) != old.get(key):
                            raise SystemExit(f"{name}: sparse reference point gate changed")
                    for key in ("values", "wa"):
                        if (key in pt) != (key in old):
                            raise SystemExit(f"{name}: sparse reference output coverage changed")
                    if "values" in pt and len(pt["values"]) != len(old["values"]):
                        raise SystemExit(f"{name}: sparse reference output shape changed")
            for pt in ref["points"].values():
                if sparse_models is None and "values" in pt and "wa" not in pt:
                    raise SystemExit(f"{name}: platform reference outputs are incomplete")
                if "values" not in pt and pt.get("status") != "REJECTED_BOTH":
                    raise SystemExit(f"{name}: platform reference refusal is missing")
            refs[name] = {**ref, "recorded": provenance}
    return refs, recorded


def runtime_compiler(check_bin):
    """Read the compiler from the tested binary, not the host's toolchain."""
    proc = subprocess.run([str(check_bin), "--compiler"], capture_output=True,
                          text=True, timeout=10)
    compiler = proc.stdout.strip()
    if proc.returncode or compiler not in ("clang", "gcc", "msvc", "unknown"):
        raise SystemExit("cannot identify runtime compiler; rebuild stanli_check "
                         "or specify --reference-compiler for an external driver")
    return compiler


def ulp_limit_for(model, ref, target_platform):
    if ref.get("recorded", {}).get("platform") == target_platform:
        return ULP_LIMITS.get(model)
    return None


def default_check_bin():
    """The stanli_check to use when the caller names none.

    Every build recipe in the project configures build-rel; build/ is the
    older debug tree that only some checkouts keep. Prefer whichever
    exists, so the corpus tools run without a flag either way. Shared by
    every tool that shells out to stanli_check, because a default that
    disagrees between the recorder and the replay is a way to record
    references from one binary and gate on another.
    """
    for name in ("build-rel", "build"):
        p = REPO / name / "stanli_check"
        if p.exists():
            return p
    return REPO / "build-rel" / "stanli_check"


def model_files(model, ref, pdb, tmp):
    """(stan, data) for a reference entry, from whichever corpus has it.

    posteriordb data is a zip per dataset and several models share one, so
    it is unpacked into tmp; the models carried in the tree have their data
    next to them and need no unpacking.
    """
    case = local_cases().get(model)
    if case is not None:
        return case.source, case.data
    # Keep the dataset the oracle recorded even if PDB adds an earlier
    # posterior for the same model after the reference was generated.
    stan = pdb / "models" / "stan" / f"{model}.stan"
    dz = pdb / "data" / "data" / f"{ref['data']}.json.zip"
    if not stan.exists() or not dz.exists():
        return stan, dz
    return stan, materialize_data(CorpusCase(model, stan, dz, "posteriordb", {}), tmp)


def ulp_distance(a, b):
    if a == b:
        return 0
    ia, ib = (struct.unpack("<q", struct.pack("<d", v))[0] for v in (a, b))
    key = lambda i: (-(1 << 63)) - i if i < 0 else i
    return abs(key(ia) - key(ib))


def pair_dev(a, b, floor=1.0):
    """(rel, ulp) for one reference/stanli pair, nonfinite-safe.

    The deviation is scaled by the larger magnitude, or by floor when both
    are smaller, so values below floor are held to an absolute error.

    Both NaN, or the same infinity, is agreement (0, 0). A nonfinite
    value on one side only is an infinite relative deviation: the old
    arithmetic produced NaN here and Python's max() silently kept the
    running value, which hid dogs_log disagreeing with CmdStan at -inf
    for months.
    """
    if a != a and b != b:
        return (0.0, 0)
    if a == b:
        return (0.0, 0)
    if not (a - a == 0.0 and b - b == 0.0):
        return (float("inf"), ulp_distance(a, b) if a == a and b == b else 0)
    scale = max(abs(a), abs(b), floor)
    return (abs(a - b) / scale, ulp_distance(a, b))


def gradient_ulp(rv, gv):
    """Largest |a-b| over the gradient, in ULPs of its largest entry."""
    if not all(math.isfinite(x) for x in rv + gv):
        return worst_pair(rv, gv)[1]
    scale = max([abs(x) for x in rv + gv] or [0.0])
    diff = max([abs(a - b) for a, b in zip(rv, gv)] or [0.0])
    return math.ceil(diff / math.ulp(scale)) if diff else 0


# The ULP gate (TESTING.md, "The rule"). Every value is held to
# DEFAULT_ULP_BOUND, counted by scaled_ulp, unless the ledger lists a
# larger bound measured for that model, output and platform. It is an
# extra condition: the scaled-error gate and the per-element ULP_LIMITS
# still apply. A value with a stored true value is held to that instead
# (truth_gate, below).
ULP_LEDGER_PATH = REPO / "tests" / "ulp_ledger.json"
ULP_LEDGER_SCHEMA = 3
# Each entry's "closer" says which engine is closer to the high-precision
# value of that output: "stanli" (a tie counts as stanli), "cmdstan" (stanli
# is farther), or "unknown" (no usable reference, one more than 1e6 scaled
# ULP from both engines, or not measured).
CLOSER = ("stanli", "cmdstan", "unknown")
DEFAULT_ULP_BOUND = 10
# A replay that measures no more than this fraction of a ledger bound says
# that the bound can be tightened.
ULP_TIGHTEN_FRACTION = 0.5
# --update-ulp-ledger lists values over the default one by one, unless one
# output kind of a model has more of them than this; then one entry covers
# the kind.
LEDGER_VALUE_ENTRIES_MAX = 8


def scaled_ulp(a, b, scale):
    """ULPs between a and b, or |a-b| in ULPs of scale if that is fewer.

    scale is the largest CmdStan magnitude among the values computed
    together with a (the whole gradient, or one write_array variable).
    A gradient entry that cancels to nearly zero is then judged against
    the size of the terms it came from, and not only against its own
    magnitude, where one rounding of a term can be 1e18 ULP. Both NaN or
    the same infinity is 0; a nonfinite value on one side only is inf.
    """
    if (a != a and b != b) or a == b:
        return 0
    if not (math.isfinite(a) and math.isfinite(b)):
        return math.inf
    ulps = ulp_distance(a, b)
    if scale > 0 and math.isfinite(scale):
        ulps = min(ulps, math.ceil(abs(a - b) / math.ulp(scale)))
    return ulps


# A ledger entry is one value's label ("lp", "grad[3]", "wa mu.2") or an
# output kind that covers every value of that kind with no entry of its own.
KIND_ENTRIES = ("grad", "wa")


def output_kind(label):
    if label == "lp":
        return "lp"
    return "grad" if label.startswith("grad[") else "wa"


def entry_kind(label):
    """The output kind of a ledger entry: its own kind for a kind entry."""
    return label if label in KIND_ENTRIES else output_kind(label)


def entry_measured(label, entries, measured):
    """The worst measurement that the ledger entry `label` covers."""
    if label in KIND_ENTRIES:
        return max([u for name, u in measured.items()
                    if output_kind(name) == label and name not in entries]
                   or [0])
    return measured.get(label, 0)


def density_ulps(rv, gv):
    """{label: scaled ULP} for one point's lp and gradient.

    lp is scored on its own magnitude, every gradient entry against the
    largest finite CmdStan gradient entry.
    """
    scale = max([abs(x) for x in rv[1:] if math.isfinite(x)] or [0.0])
    out = {}
    for i, (a, b) in enumerate(zip(rv, gv)):
        ulps = scaled_ulp(a, b, abs(a) if i == 0 else scale)
        if ulps:
            out["lp" if i == 0 else f"grad[{i}]"] = ulps
    return out


def write_array_ulps(names, wref, wgot):
    """{"wa COLUMN": scaled ULP} for one write_array row, each column scored
    against the largest finite CmdStan value of its own variable."""
    variable = [re.split(r"[.:]", name, maxsplit=1)[0] for name in names]
    scale = collections.defaultdict(float)
    for var, a in zip(variable, wref):
        if math.isfinite(a):
            scale[var] = max(scale[var], abs(a))
    out = {}
    for name, var, a, b in zip(names, variable, wref, wgot):
        ulps = scaled_ulp(a, b, scale[var])
        if ulps:
            out[f"wa {name}"] = ulps
    return out


def ulp_bound(entries, label):
    """(bound, reason) for one value: its own ledger entry, else its
    output kind's, else the default."""
    entry = entries.get(label)
    if entry is None and output_kind(label) in KIND_ENTRIES:
        entry = entries.get(output_kind(label))
    if entry is None:
        return DEFAULT_ULP_BOUND, "default"
    return entry["bound"], entry["reason"]


def ulp_gate(entries, ulps, point, values=None, skip=()):
    """A ("ULP_GATE", detail) failure for the first value over its bound,
    or None. The values in `skip` have a true value, and truth_gate holds
    them instead."""
    for label, ulps_here in ulps.items():
        if label in skip:
            continue
        bound, reason = ulp_bound(entries, label)
        if ulps_here > bound:
            shown = ""
            if values and label in values:
                shown = f" (stanli {values[label][1]!r}, CmdStan {values[label][0]!r})"
            return ("ULP_GATE", f"point {point}: {label}{shown} is {ulps_here} "
                                f"scaled ULP from CmdStan, allowed {bound} "
                                f"({reason})")
    return None


# True values (TESTING.md, "The rule"). The ledger's "true_values" maps
# corpus -> model -> entry label -> {bound, method, platform, points}. They
# do not depend on the platform, so they sit outside "platforms". `points`
# maps each point to {value label: {"value": the 80-digit value as a decimal
# string, "scale": the scale of its scaled ULP}}. An entry for one value has
# one label per point. A kind entry ("grad" or "wa") lists the values that it
# covered when it was measured. `bound` is the largest distance of stanli
# from the true value over the points, measured on `platform`. `method` is
# "fd" for a gradient entry (a full finite-difference gradient) and "direct"
# for lp and outputs.
#
# A value with a true value is held to within max(bound, DEFAULT_ULP_BOUND)
# scaled ULP of it on every platform, and its distance from CmdStan is not
# gated. tools/hp/ledger_accuracy.py --write-true-values writes the entries.
TRUTH_KEYS = {"bound", "method", "platform", "points"}


def truth_ulps(x, value, scale):
    """Scaled ULPs of stanli's x from the stored true value (a decimal
    string), in ULPs of scale. The same measure as ledger_accuracy.py."""
    return hp_metrics.ulps_from_truth(x, hp_metrics.exact(value), scale)


def truth_section(ledger, corpus):
    """{model: {entry label: true-value entry}} for one corpus."""
    return ledger.get("true_values", {}).get(corpus, {})


def truth_labels(truth, point=None):
    """The value labels that one model's true values cover at `point`, or
    at any point, with the entry labels. These values are held to the true
    value and not to CmdStan."""
    labels = set(truth or {}) if point is None else set()
    for entry in (truth or {}).values():
        for at, values in entry["points"].items():
            if point is None or at == str(point):
                labels.update(values)
    return labels


def truth_bound(entry):
    return max(entry["bound"], DEFAULT_ULP_BOUND)


def truth_gate(truth, point, got, measured=None):
    """A ("TRUTH_GATE", detail) failure for the first value at `point` that
    is farther from its true value than its entry allows, or None.

    got maps value labels to stanli's values. Every value with a true value
    at this point is checked, also one that equals CmdStan's. measured, when
    given, keeps the largest distance seen for each entry label.
    """
    for label, entry in sorted((truth or {}).items()):
        for name, stored in sorted(entry["points"].get(str(point), {}).items()):
            if name not in got:
                return ("TRUTH_GATE", f"point {point}: {name} has a true value "
                                      f"and stanli gave no value for it")
            x = got[name]
            ulps = truth_ulps(x, stored["value"], stored["scale"])
            if measured is not None:
                measured[label] = max(measured.get(label, 0.0), ulps)
            if ulps > truth_bound(entry):
                return ("TRUTH_GATE",
                        f"point {point}: {name} (stanli {x!r}, true value "
                        f"{stored['value'][:22]}) is {ulps:.1f} scaled ULP "
                        f"from the true value, allowed {truth_bound(entry)} "
                        f"(the bound of {label} is {entry['bound']}, measured "
                        f"on {entry['platform']})")
    return None


def truth_problems(ledger):
    """Why the "true_values" of a ledger are malformed, if they are."""
    problems = []
    for corpus, models in ledger.get("true_values", {}).items():
        for model, entries in models.items():
            for label, entry in entries.items():
                where = f"true_values {corpus} {model} {label}"
                if not isinstance(entry, dict) or set(entry) != TRUTH_KEYS:
                    problems.append(f"{where} needs exactly "
                                    f"{', '.join(sorted(TRUTH_KEYS))}")
                    continue
                bound = entry["bound"]
                if isinstance(bound, bool) or not isinstance(bound, int) \
                        or bound < 0:
                    problems.append(f"{where} needs a bound that is an "
                                    f"integer of 0 or more")
                want = "fd" if entry_kind(label) == "grad" else "direct"
                if entry["method"] != want:
                    problems.append(f"{where} needs method {want!r}")
                platform_entry = ledger["platforms"].get(
                    entry["platform"], {}).get(corpus, {}).get(model, {}).get(label)
                if platform_entry is None or platform_entry["closer"] != "stanli":
                    problems.append(f"{where} needs an entry with closer "
                                    f"stanli on {entry['platform']}")
                points = entry["points"]
                if not (isinstance(points, dict) and points
                        and set(points) <= {str(p) for p in POINTS}):
                    problems.append(f"{where} needs points among "
                                    f"{', '.join(map(str, POINTS))}")
                    continue
                for point, values in points.items():
                    if not (isinstance(values, dict) and values):
                        problems.append(f"{where} point {point} has no values")
                        continue
                    for name, stored in values.items():
                        if not (name == label or (label in KIND_ENTRIES
                                                  and output_kind(name) == label)):
                            problems.append(f"{where} point {point} lists "
                                            f"{name}, which {label} does not cover")
                        if not truth_value_ok(stored):
                            problems.append(f"{where} point {point} {name} needs "
                                            f"a finite decimal value and a "
                                            f"positive finite scale")
    return problems


def truth_value_ok(stored):
    if not (isinstance(stored, dict) and set(stored) == {"value", "scale"}
            and isinstance(stored["value"], str)
            and isinstance(stored["scale"], float)
            and math.isfinite(stored["scale"]) and stored["scale"] > 0):
        return False
    try:
        return hp_metrics.exact(stored["value"]) is not None
    except ArithmeticError:
        return False


def load_ulp_ledger(path=ULP_LEDGER_PATH):
    if not path.exists():
        return {"schema": ULP_LEDGER_SCHEMA, "platforms": {}, "true_values": {}}
    ledger = json.loads(path.read_text())
    if ledger.get("schema") != ULP_LEDGER_SCHEMA:
        raise SystemExit(f"{path} is schema {ledger.get('schema')!r}, "
                         f"these tools read {ULP_LEDGER_SCHEMA}")
    if set(ledger) - {"schema", "platforms", "true_values"}:
        raise SystemExit(f"{path} has keys other than schema, platforms and "
                         f"true_values")
    ledger.setdefault("true_values", {})
    for platform_name, corpora in ledger["platforms"].items():
        for corpus, models in corpora.items():
            for model, entries in models.items():
                for label, entry in entries.items():
                    if not (isinstance(entry.get("bound"), int)
                            and entry["bound"] > DEFAULT_ULP_BOUND
                            and entry.get("reason")
                            and entry.get("closer") in CLOSER):
                        raise SystemExit(
                            f"{path}: {platform_name} {corpus} {model} {label} "
                            f"needs an integer bound over {DEFAULT_ULP_BOUND}, "
                            f"a reason and a closer of {', '.join(CLOSER)}")
    problems = truth_problems(ledger)
    if problems:
        raise SystemExit(f"{path}: " + "; ".join(problems[:5]))
    return ledger


def ulp_section(ledger, target_platform, corpus):
    """{model: entries} for this platform and corpus, or None when the
    ledger has never been measured there and the ULP gate is off."""
    return ledger["platforms"].get(target_platform, {}).get(corpus)


def ledger_notes(model, entries, measured, truth=None, truth_measured=None):
    """Notes for ledger entries that this replay beats by a wide margin.

    An entry with true values gets a note only for its distance from the
    true value, and only when its bound is over the default.
    """
    notes = []
    for label, entry in sorted((truth or {}).items()):
        got = (truth_measured or {}).get(label)
        if got is not None and entry["bound"] > DEFAULT_ULP_BOUND \
                and got <= entry["bound"] * ULP_TIGHTEN_FRACTION:
            notes.append(f"TRUE VALUE {model} {label}: measured {got:.1f} "
                         f"from the true value, recorded bound "
                         f"{entry['bound']}; rerun with --update-ulp-ledger "
                         f"on {entry['platform']} to lower it")
    for label, entry in sorted(entries.items()):
        if label in (truth or {}):
            continue
        got = entry_measured(label, entries, measured)
        if got <= entry["bound"] * ULP_TIGHTEN_FRACTION:
            notes.append(f"ULP LEDGER {model} {label}: measured {got}, "
                         f"recorded bound {entry['bound']}; rerun with "
                         f"--update-ulp-ledger to tighten it")
    return notes


def updated_entries(old, measured, reason, truth=frozenset()):
    """The ledger entries for one model after a replay that passed.

    Existing entries keep their reason and closer and can only go down: to
    the new measurement, or away once it is within the default. A value over
    the default that no entry covers gets a new entry with `reason` and
    closer "unknown". A value over its bound is an error: the gate fails it
    before this runs.

    truth is truth_labels() of the model. The gate does not hold those
    values to CmdStan, so their entries stay as they are, and they get no
    new entries.
    """
    new = {label: dict(entry) for label, entry in old.items() if label in truth}
    measured = {label: got for label, got in measured.items()
                if label not in truth}
    for label, entry in old.items():
        if label in truth:
            continue
        got = entry_measured(label, old, measured)
        if got > entry["bound"]:
            raise ValueError(f"{label} measured {got} over bound {entry['bound']}")
        if got > DEFAULT_ULP_BOUND:
            new[label] = {"bound": int(got), "reason": entry["reason"],
                          "closer": entry["closer"]}
    for label, (bound, count) in uncovered_bounds(old, measured).items():
        new[label] = {"bound": bound, "reason": reason + count,
                      "closer": "unknown"}
    return new


def uncovered_bounds(old, measured):
    """{label: (bound, count note)} for the values over the default that no
    entry in `old` covers: one per value, or one per output kind when a
    kind has more than LEDGER_VALUE_ENTRIES_MAX of them."""
    over = collections.defaultdict(dict)
    for label, got in measured.items():
        if got > DEFAULT_ULP_BOUND and label not in old \
                and output_kind(label) not in old:
            over[output_kind(label)][label] = got
    bounds = {}
    for kind, values in over.items():
        if len(values) > LEDGER_VALUE_ENTRIES_MAX:
            bounds[kind] = (int(max(values.values())),
                            f"; {len(values)} {kind} values over "
                            f"{DEFAULT_ULP_BOUND}")
        else:
            bounds.update((label, (int(got), "")) for label, got in values.items())
    return bounds


# --add-ulp-entries gives a model new ledger entries, with the reasons that
# a person wrote. It is separate from --update-ulp-ledger, which only lowers
# bounds, because the ledger cannot tell a new model, or a value that moved
# for a known reason, from a regression: only the person who names the
# model can.
def first_entries(measured, reasons, target_platform, old=None,
                  truth=frozenset()):
    """(entries, problems) for a model named in the --add-ulp-entries file.

    reasons maps labels that the replay measured over the default to why:
    a reason string, or {"reason": str, "closer": one of CLOSER}; a string
    means closer "unknown". Every such label needs a reason, unless `old`
    (the model's current entries) has an entry for that label that the
    measurement is within: that entry keeps its reason and closer, at the
    measured bound. A label with neither, or a reason for a label within
    the default, is a problem, and nothing may be written.

    truth is truth_labels() of the model. Those values are held to their
    true values, so their entries in `old` are kept as they are, and they
    need no reason.
    """
    old = old or {}
    given = {label: why if isinstance(why, dict)
             else {"reason": why, "closer": "unknown"}
             for label, why in reasons.items()}
    bounds = uncovered_bounds({}, {label: got for label, got in measured.items()
                                   if label not in truth})
    kept = {label: {"bound": bound, "reason": old[label]["reason"],
                    "closer": old[label]["closer"]}
            for label, (bound, _) in bounds.items()
            if label not in reasons and label in old
            and bound <= old[label]["bound"]}
    kept.update({label: dict(entry) for label, entry in old.items()
                 if label in truth})
    problems = [f"{label} measured {bounds[label][0]} needs a reason"
                for label in sorted(set(bounds) - set(reasons) - set(kept))]
    problems += [f"{label} has a reason but is within {DEFAULT_ULP_BOUND}"
                 for label in sorted(set(reasons) - set(bounds))]
    entries = {label: {"bound": bound,
                       "reason": f"measured on {target_platform}; "
                                 f"{given[label]['reason']}{count}",
                       "closer": given[label]["closer"]}
               for label, (bound, count) in bounds.items() if label in given}
    return {**kept, **entries}, problems


def first_entry_models(reasons, section, models):
    """Why the models named for first entries cannot have them, if any."""
    if section is None:
        return ["the ledger has no section here; record it with "
                "--update-ulp-ledger"]
    return [f"{model} is not in this replay"
            for model in sorted(set(reasons) - set(models))]


def first_entry_reason(why):
    """Whether `why` is a reason string or {"reason": str, "closer": ...}."""
    if isinstance(why, dict):
        if set(why) != {"reason", "closer"} or why["closer"] not in CLOSER:
            return False
        why = why["reason"]
    return isinstance(why, str) and bool(why)


def load_first_entry_reasons(path):
    """{model: {label: reason or {"reason", "closer"}}} from the
    --add-ulp-entries file."""
    reasons = json.loads(pathlib.Path(path).read_text())
    if not (isinstance(reasons, dict) and all(
            isinstance(labels, dict)
            and all(map(first_entry_reason, labels.values()))
            for labels in reasons.values())):
        raise SystemExit(
            f"{path} must map each model to {{label: reason}}, where a "
            f"reason is a string or {{\"reason\": string, \"closer\": "
            f"one of {', '.join(CLOSER)}}}")
    return reasons


def add_first_entries(ledger, target_platform, corpus, measured, reasons,
                      path=ULP_LEDGER_PATH):
    """Replace the entries of the models in `reasons`, or write nothing.

    Returns the problems that stopped the write, if any.
    """
    section = ledger["platforms"][target_platform][corpus]
    truth = truth_section(ledger, corpus)
    problems, added = [], {}
    for model, labels in sorted(reasons.items()):
        entries, bad = first_entries(measured.get(model, {}), labels,
                                     target_platform, section.get(model),
                                     truth_labels(truth.get(model)))
        problems += [f"{model}: {problem}" for problem in bad]
        added[model] = dict(sorted(entries.items()))
    if problems:
        return problems
    section.update({model: entries for model, entries in added.items()
                    if entries})
    for model in [model for model, entries in added.items() if not entries]:
        section.pop(model, None)
    ledger["platforms"][target_platform][corpus] = dict(sorted(section.items()))
    path.write_text(json.dumps(ledger, indent=1, sort_keys=True) + "\n")
    return []


def write_ulp_ledger(ledger, target_platform, corpus, measured, ran,
                     provenance, path=ULP_LEDGER_PATH, truth_measured=None):
    """Ratchet this platform's section for the models that ran.

    measured is {model: {label: worst scaled ULP over the points}}.
    truth_measured is {model: {entry label: worst distance from the true
    value}}. A true-value bound can only go down, and only on the platform
    that measured it.
    """
    section = ledger["platforms"].setdefault(target_platform, {}).setdefault(
        corpus, {})
    truth = truth_section(ledger, corpus)
    for model in ran:
        for label, entry in truth.get(model, {}).items():
            got = (truth_measured or {}).get(model, {}).get(label)
            if got is not None and entry["platform"] == target_platform:
                entry["bound"] = min(entry["bound"], math.ceil(got))
        old = section.get(model, {})
        recorded_on = provenance.get(model)
        reason = (f"measured on {target_platform}"
                  + (f" against a reference recorded on {recorded_on}"
                     if recorded_on and recorded_on != target_platform else "")
                  + "; cause not yet explained")
        entries = updated_entries(old, measured.get(model, {}), reason,
                                  truth_labels(truth.get(model)))
        if entries:
            section[model] = dict(sorted(entries.items()))
        else:
            section.pop(model, None)
    ledger["platforms"][target_platform][corpus] = dict(sorted(section.items()))
    path.write_text(json.dumps(ledger, indent=1, sort_keys=True) + "\n")


def record_ulps(measured, ulps):
    if measured is not None:
        for label, got in ulps.items():
            measured[label] = max(measured.get(label, 0), got)


def run_check(cmd, timeout=None):
    """(process, CheckStatus) for one stanli_check run.

    The status file holds what the run found out beyond the values on
    stdout; see tools/check_status.py.
    """
    fd, path = tempfile.mkstemp(prefix="stanli_status_", suffix=".jsonl")
    os.close(fd)
    try:
        proc = subprocess.run([str(a) for a in cmd] + ["--status-json", path],
                              capture_output=True, text=True, cwd=REPO,
                              timeout=timeout)
        return proc, check_status.read(path)
    finally:
        pathlib.Path(path).unlink(missing_ok=True)


def parse_wa(out):
    """(names_csv, value_strings) from WANAMES/WAVALS lines, or None."""
    names, vals = None, None
    for line in out.splitlines():
        if line.startswith("WANAMES "):
            names = line[8:]
        elif line.startswith("WAVALS"):
            vals = line[6:].split()
    if names is None or vals is None or names.startswith("FAIL"):
        return None
    if vals and vals[0] == "FAIL":
        return None
    return (names, vals)


def parse_status(out):
    """Fields of the final machine-readable result line, or [].

    stanli_check and ref_driver both print it after any Stan print output,
    and a user `print("OK ...")` must not shadow it, so read backwards.
    """
    for line in reversed(out.splitlines()):
        fields = line.split()
        if fields and fields[0] in ("OK", "COMPILE_FAIL", "EVAL_FAIL", "DATA_FAIL"):
            return fields
    return []


def accepted(fields):
    """Did this engine accept the point, per its own way of saying no?

    The two spell a rejection differently. stanli_check prints EVAL_FAIL
    and no OK line when evaluation threw, and prints the values when it
    did not, so a point it cannot evaluate also arrives as OK with a row
    of nan. CmdStan's log_prob hands back that same row and only throws
    later, out of write_array, so ref_driver prints OK first. A
    unit_vector at the origin is one; s2_invgaussian is another, since
    its inverse-square link takes inv_sqrt of a linear predictor that no
    evaluation point keeps positive. Reading the all-nan row as a value
    would record a reference no engine can be held to.
    """
    if not fields or fields[0] != "OK":
        return False
    values = [float(x) for x in fields[1:]]
    return not (values and all(v != v for v in values))


def corpus_models(pdb, wanted=(), contains="", excluded=()):
    """Unique shared-corpus (model, data-name) pairs."""
    wanted, excluded = set(wanted), set(excluded)
    for model, case in corpus_cases(pdb, include_language=True).items():
        if (model in excluded or (wanted and model not in wanted)
                or (contains and contains not in model)):
            continue
        yield model, case.metadata.get("data_name")


def corpus_input(pdb, tmp, model, data_name):
    stan, data = model_files(model, {"data": data_name}, pdb, tmp)
    return (stan, data) if stan.exists() and data.exists() else None


def cmdstan_header(cmdstan, work, stan, data):
    """One CmdStan CSV header, less the seven sampler columns."""
    local = work / stan.name
    local.write_bytes(stan.read_bytes())
    exe = work / stan.stem
    built = subprocess.run(["make", str(exe), f"STANC={toolchain.PINNED_STANC}"],
                           cwd=cmdstan,
                           capture_output=True, text=True)
    if built.returncode != 0:
        lines = built.stderr.strip().splitlines()
        return None, "build failed: " + (lines[-1][:120] if lines else "")
    csv = work / f"{stan.stem}.csv"
    run = subprocess.run(
        [str(exe), "sample", "num_warmup=2", "num_samples=1", "data",
         f"file={data}", "output", f"file={csv}"],
        cwd=work, capture_output=True, text=True)
    if not csv.exists():
        return None, "run failed: " + (run.stdout + run.stderr).strip()[-160:]
    for line in csv.read_text().splitlines():
        if line and not line.startswith("#"):
            return line.split(",")[N_SAMPLER_COLS:], ""
    return None, "no header in csv"


def check_wa_headers(pdb, check_bin, cmdstan, models, excluded=()):
    """Compare write_array columns with a live CmdStan, model by model."""
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_hdr_"))
    ok, bad, skipped = [], [], []
    selected = list(corpus_models(pdb, models, excluded=excluded))
    if not selected:
        print("no corpus models selected", file=sys.stderr)
        return 2
    runnable = 0
    for model, data_name in selected:
        inputs = corpus_input(pdb, tmp, model, data_name)
        if inputs is None:
            continue
        runnable += 1
        stan, data = inputs
        ours, status = run_check([check_bin, stan, data, "--columns"])
        if ours.returncode != 0:
            skipped.append((model, ours.stdout.strip()[:110]))
            continue
        if status.compile is None:
            skipped.append((model, "stanli_check wrote no status record"))
            continue
        work = tmp / model
        work.mkdir()
        theirs, why = cmdstan_header(cmdstan, work, stan, data)
        if theirs is None:
            skipped.append((model, why))
            continue
        got = ours.stdout.strip().split(",")
        truncated = status.write_array().get("truncated") is not None
        ref = theirs[:len(got)] if truncated else theirs
        if got == ref:
            ok.append((model, len(got), truncated))
        else:
            first = next((i for i in range(max(len(got), len(ref)))
                          if i >= len(got) or i >= len(ref) or got[i] != ref[i]),
                         0)
            bad.append((model, len(got), len(ref), first,
                        got[first:first + 4], ref[first:first + 4]))
    print(f"\n== {len(ok)} match, {len(bad)} differ, {len(skipped)} skipped ==")
    for model, n, truncated in ok:
        print(f"  MATCH   {model:44s} {n:6d} cols"
              + ("  (prefix)" if truncated else ""))
    for model, no, nr, first, got, ref in bad:
        print(f"  DIFFER  {model:44s} ours {no} cols, cmdstan {nr}; "
              f"first difference at {first}\n      ours    {got}\n      cmdstan {ref}")
    for model, why in skipped:
        print(f"  skip    {model:44s} {why}")
    if runnable == 0 or not (ok or bad):
        print("no selected model was checked", file=sys.stderr)
        return 2
    return 1 if bad else 0


def fail_detail(proc, got):
    """Why a stanli_check run did not print OK.

    Say HOW it died, not just that it did: a negative returncode is a
    signal (ldaK5's 49 GB compile came back as a bare RUN_FAIL because
    the OOM killer leaves no stdout).
    """
    detail = " ".join(got[:3])
    if not detail:
        detail = (f"killed by signal {-proc.returncode}"
                  if proc.returncode < 0
                  else f"exit {proc.returncode}, no output")
    err = proc.stderr.strip().splitlines()
    if err:
        detail += " | " + err[-1][:120]
    return detail


def probe_point(model, stan, dj, check_bin, point, timeout,
                fast_math=False, opts=None):
    """Evaluate a point with no recorded reference. (status, detail).

    No CmdStan values means no parity check, so the bar here is only what
    the run can be held to on its own evidence:

      * It must not crash. A signal, or a nonzero exit with none of
        stanli_check's machine-readable lines on stdout, is a hard
        failure. This is the whole reason the extra points are run: a
        SIGSEGV is not a numerical disagreement anyone needs a reference
        to adjudicate.
      * A clean rejection passes. stanli_check prints EVAL_FAIL only when
        evaluation THREW, which is how a model outside its declared
        support at this point reports itself (an ODE solution dipping
        below a lower bound, say). CmdStan's ref_driver throws in the
        same place, and the recorder counts both engines refusing a point
        as agreement rather than a stanli failure -- REJECTED_BOTH in
        verify_sample.py. Only one engine is present here, so the
        rejection is accepted, not confirmed.
      * An lp of -inf passes for the same reason: a zero density is a
        value both engines produce and agree on (bernoulli_lccdf(1|theta)
        is log(0) by definition, and stanli_check deliberately prints
        nonfinite values rather than refusing them so the two drivers
        stay comparable). +inf or NaN does not pass -- no reference in
        the corpus was recorded that way, and a NaN lp is exactly what an
        unwritten register reads as.
      * A finite lp must come with finite gradients. lp finite next to a
        nonfinite gradient is the shape a dropped tape link takes, and
        spotting it needs no reference. NONFINITE_GRAD_OK and QUARANTINED
        carry the (model, point) pairs excused from this, each with the
        CmdStan run that settled it.

    Every point of every model is referenced today, so this runs only for
    a quarantined point or a reference file with a gap in it.

    COMPILE_FAIL is a failure here rather than a rejection: compiling does
    not depend on the evaluation point, so a model that compiled at its
    recorded point must compile at every other one.
    """
    try:
        proc = subprocess.run(
            [str(check_bin), str(stan), str(dj), "--point", str(point)]
            + (["--fast-math"] if fast_math else [])
            + (list(opts.args) if opts else []),
            capture_output=True, text=True, cwd=REPO, timeout=timeout)
    except subprocess.TimeoutExpired:
        return ("POINT_TIMEOUT", "")
    got = parse_status(proc.stdout)
    kind = got[0] if got else ""
    if kind == "EVAL_FAIL":
        return ("OK", "")
    if kind != "OK":
        return ("POINT_COMPILE_FAIL" if kind == "COMPILE_FAIL" else "CRASH",
                fail_detail(proc, got))
    lp = float(got[1])
    if math.isnan(lp) or lp == math.inf:
        return ("POINT_NONFINITE_LP", f"lp {got[1]}")
    if lp == -math.inf:
        return ("OK", "")
    bad = sum(1 for x in got[2:] if not math.isfinite(float(x)))
    excused = ((model, point) in NONFINITE_GRAD_OK
               or (model, point) in QUARANTINED)
    if bad and not excused:
        return ("POINT_NONFINITE_GRAD",
                f"lp {got[1]} but {bad}/{len(got) - 2} gradients nonfinite")
    return ("OK", "")


def worst_pair(rv, gv, floor=1.0):
    """(worst relative deviation, worst ULP distance) over paired values."""
    worst, worst_ulp = 0.0, 0
    for a, b in zip(rv, gv):
        rel, ulp = pair_dev(a, b, floor)
        worst = max(worst, rel)
        worst_ulp = max(worst_ulp, ulp)
    return worst, worst_ulp


# Fast mode (TESTING.md, "Fast mode"): lp and the gradient are held to
# FAST_MAX_REL, the gradient as a whole against its largest entry; every
# other output keeps the default gate.
FAST_MAX_REL = 1e-12
WA_MAX_REL = 1e-9
# Fast-mode points allowed past FAST_MAX_REL, each with the limit it is held
# to and the 80-digit reference result that justifies it.
FAST_HP_CHECKED = {
    "epinow2_estimate_infections": (
        1e-10, "point 0: the 14-day generation-time PMF tail loses 7 digits "
               "in double; fast is 8.651e-9 of the largest gradient entry "
               "from the 80-digit gradient, CmdStan 8.646e-9"),
}


def fast_dev(rv, gv):
    """(worst deviation, worst ULP distance) under the fast-mode metric.

    Element 0 is lp, scored as pair_dev scores it. The rest is the
    gradient, scored as one vector: the largest absolute difference over
    the largest absolute entry on either side (at least 1). A coordinate
    that cancels to nearly zero is then judged against the gradient it
    belongs to and not against itself. A coordinate that is nonfinite on
    one side only, or in a different way, is an infinite deviation, as in
    pair_dev.
    """
    worst, worst_ulp = pair_dev(rv[0], gv[0]) if rv else (0.0, 0)
    diff, scale = 0.0, 1.0
    for a, b in zip(rv[1:], gv[1:]):
        rel, ulp = pair_dev(a, b)
        worst_ulp = max(worst_ulp, ulp)
        if rel == math.inf:
            return (math.inf, worst_ulp)
        if a - a == 0.0 and b - b == 0.0:
            diff = max(diff, abs(a - b))
            scale = max(scale, abs(a), abs(b))
    return (max(worst, diff / scale), worst_ulp)


def density_interpreted(status):
    """Does anything but the write_array section run through the
    interpreter? A fallback that the section names as its own truncation
    belongs to it; any other one counts against the log density."""
    own = status.write_array().get("truncated")
    return any(note != own
               for note in (status.compile or {}).get("interpreter_fallbacks", []))


def check_point(model, stan, dj, check_bin, point, pt, timeout, no_wa,
                strict=False, fast_math=False, opts=None, off_platform=False,
                ulp_entries=None, measured=None, floored=None, notes=None,
                wa_gap=False, truth=None, truth_measured=None):
    """Replay one recorded point. (status, worst, worst_ulp, n, detail).

    truth, when given, is the model's entries in the ledger's true_values.
    Their values are held to the true value (truth_gate) and not to CmdStan
    (ulp_gate), and truth_measured keeps their distances from it.

    wa_gap (a WRITE_ARRAY gap, replayed with no_wa) lets the write_array
    section run through the interpreter, and nothing else
    (density_interpreted).

    ulp_entries, when not None, are the model's ULP ledger entries, and
    every value is held to its bound (ulp_gate). measured, when given,
    collects each value's scaled ULP distance, keeping the worst seen.
    floored (a Floored) holds some lp/gradient values to their own
    scaled-error gate, and keeps them out of the worst deviation that the
    caller gates; a nonzero deviation there is added to notes.

    A point whose entry has no `values` is one CmdStan itself refuses (it
    threw, or answered with a row of nan, which is how a unit_vector at
    the origin reports itself). The reference records the refusal, and
    stanli has to refuse it too, in either of the two spellings the
    recorder reads as one: EVAL_FAIL, which stanli_check prints when
    evaluation threw, or an OK line whose values are all nan. Accepting a
    point CmdStan rejects is a real disagreement, not a free pass -- it is
    the same asymmetry as one engine throwing, read from the other side.

    On a passing point, detail names the values with the worst scaled
    error and the worst ULP distance, for the gate replay_model applies.
    """
    cmd = [str(check_bin), str(stan), str(dj), "--point", str(point)]
    if fast_math:
        cmd.append("--fast-math")
    if opts:
        cmd += list(opts.args)
    want_wa = "values" in pt and not no_wa
    if want_wa:
        cmd.append("--wa-values")
    try:
        proc, status = run_check(cmd, timeout)
    except subprocess.TimeoutExpired:
        return ("TIMEOUT", 0.0, 0, 0, f"point {point}")
    got = parse_status(proc.stdout)
    kind = got[0] if got else ""
    if "values" not in pt:
        if kind == "EVAL_FAIL":
            return ("OK", 0.0, 0, 0, "")
        if kind == "OK":
            if not accepted(got):
                return ("OK", 0.0, 0, 0, "")
            return ("POINT_NOT_REJECTED", 0.0, 0, 0,
                    f"point {point}: CmdStan rejects it, stanli returned "
                    f"lp {got[1]}")
        # Refusing for the wrong reason is not agreement: COMPILE_FAIL
        # says the model never ran, and a crash says nothing at all.
        return ("POINT_COMPILE_FAIL" if kind == "COMPILE_FAIL" else "CRASH",
                0.0, 0, 0, f"point {point}: {fail_detail(proc, got)}")
    if kind != "OK":
        # A death with nothing to say (a signal, or a nonzero exit and no
        # machine-readable line) is reported as a crash even here, where a
        # reference exists: "stanli disagrees with CmdStan" and "stanli
        # segfaulted" are different bugs and reading the first one for the
        # second is how reductions_allowed stayed green for a month.
        crashed = proc.returncode < 0 or not got
        return ("CRASH" if crashed else "RUN_FAIL", 0.0, 0, 0,
                f"point {point}: {fail_detail(proc, got)}")
    if status.compile is None:
        return ("CRASH", 0.0, 0, 0,
                f"point {point}: stanli_check wrote no status record")
    expected = opts.interpreted if opts else model in CORPUS_INTERPRETED
    interpreted = density_interpreted(status) if wa_gap else status.interpreted
    if interpreted != expected:
        parts = (status.compile["interpreter_fallbacks"]
                 or ["write_array section"])
        return ("INTERPRETER", 0.0, 0, 0,
                f"point {point}: " + (f"runs through the MIR interpreter: "
                                      f"{', '.join(parts)[:200]}"
                                      if not expected
                                      else "expected the MIR interpreter"))
    rv = [float(x) for x in pt["values"]]
    gv = [float(x) for x in got[1:]]
    if len(rv) != len(gv):
        return ("SHAPE_FAIL", 0.0, 0, 0,
                f"point {point}: {len(rv)} vs {len(gv)}")
    if strict and not all(math.isfinite(x) for x in rv + gv):
        return ("NONFINITE", 0.0, 0, 0, f"point {point}: lp/gradient is nonfinite")
    floor = opts.scale_floor if opts else 1.0
    own = opts.value_limits if opts else {}
    ulps = density_ulps(rv, gv)
    record_ulps(measured, ulps)
    held = truth_labels(truth, point)
    labels = {("lp" if i == 0 else f"grad[{i}]"): (a, b)
              for i, (a, b) in enumerate(zip(rv, gv))}
    if ulp_entries is not None:
        failed = ulp_gate(ulp_entries, ulps, point, labels, held)
        if failed:
            return (failed[0], 0.0, max(ulps.values()), len(rv), failed[1])
    if truth:
        failed = truth_gate({label: entry for label, entry in truth.items()
                             if entry_kind(label) != "wa"}, point,
                            {label: b for label, (_, b) in labels.items()},
                            truth_measured)
        if failed:
            return (failed[0], 0.0, max(ulps.values(), default=0), len(rv),
                    failed[1])
    # The scaled-error gate reads `scored`, in which the values that have
    # their own floor (floored_outputs) are set to CmdStan's after being
    # held to that floor here. Every ULP check still reads gv.
    scored = gv
    if floored:
        held = [i for i in floored.indices if i < len(rv)]
        fworst = max([pair_dev(rv[i], gv[i])[0] for i in held] or [0.0])
        for i in held:
            dev = pair_dev(rv[i], gv[i])[0]
            if dev >= floored.gate_of(i):
                return ("GATE", dev, 0, len(rv),
                        f"point {point}: {dev:.2e} at "
                        f"{density_label(i, opts)}, allowed "
                        f"{floored.gate_of(i):.1e} ({floored.reason})")
        if fworst >= WA_MAX_REL and notes is not None:
            lp_gate = (f", lp {floored.lp_gate:.1e}" if floored.lp_gate
                       else "")
            notes.append(f"FLOORED {model} point {point}: {fworst:.2e} over "
                         f"{len(held)} values allowed {floored.gate:.1e}"
                         f"{lp_gate} ({floored.reason})")
        scored = [rv[i] if i in floored.indices else v for i, v in enumerate(gv)]
    if fast_math:
        worst, worst_ulp = fast_dev(rv, scored)[0], fast_dev(rv, gv)[1]
    else:
        worst, worst_ulp = worst_pair(rv, scored, floor)[0], worst_pair(rv, gv, floor)[1]
        if own:
            worst_ulp = worst_pair(*([v for i, v in enumerate(side)
                                      if i not in own] for side in (rv, gv)),
                                   floor)[1]
        if model in GRADIENT_AGAINST_LARGEST or (
                off_platform and model in GRADIENT_AGAINST_LARGEST_OFF_PLATFORM):
            worst_ulp = max(worst_pair(rv[:1], gv[:1])[1],
                            gradient_ulp(rv[1:], gv[1:]))
    where = (worst_values(rv, scored, floor, own,
                          lambda i: density_label(i, opts))[0],
             worst_values(rv, gv, floor, own,
                          lambda i: density_label(i, opts))[1])
    for i, (limit, reason, expected) in own.items():
        label = density_label(i, opts)
        ulp = pair_dev(rv[i], gv[i], floor)[1]
        if ulp > limit:
            return ("ULP_GATE", worst, ulp, len(rv),
                    f"point {point}: {label} {ulp} ULP, allowed {limit} "
                    f"({reason})")
        # The wider limit holds only while an independent value agrees.
        wa = parse_wa(proc.stdout)
        want = expected([float(x) for x in wa[1]]) if wa else None
        if want is None or pair_dev(want, gv[i])[1] > 2:
            return ("ULP_GATE", worst, ulp, len(rv),
                    f"point {point}: {label} {gv[i]!r} is not within 2 ULP "
                    f"of {want!r}")
    n = len(rv)
    if want_wa:
        # The write_array reference: column names must match exactly, and
        # the values (constrained parameters, transformed parameters and
        # generated quantities at the same point) share the model's gate.
        #
        # A row is demanded even where the reference holds none. The
        # recorder drops the `wa` block whenever the two engines disagreed
        # about it, stanli's included, so a model whose write_array fails
        # outright records no reference and used to be replayed as if it
        # had no section at all. Every model has a row: the constrained
        # parameters are one.
        wa = parse_wa(proc.stdout)
        if wa is None:
            return ("WA_FAIL", worst, worst_ulp, n,
                    f"point {point}: no or failed write_array output")
        if truth:
            failed = truth_gate({label: entry for label, entry in truth.items()
                                 if entry_kind(label) == "wa"}, point,
                                {f"wa {c}": float(x) for c, x in
                                 zip(wa[0].split(","), wa[1])},
                                truth_measured)
            if failed:
                return (failed[0], worst, worst_ulp, n, failed[1])
        if "wa" not in pt:
            return ("OK", worst, worst_ulp, n, where)
        names, vals = wa
        if names != pt["wa"]["names"]:
            return ("WA_NAMES_FAIL", worst, worst_ulp, n,
                    f"point {point}: got {names[:60]} "
                    f"want {pt['wa']['names'][:60]}")
        wref = [float(x) for x in pt["wa"]["values"]]
        wgot = [float(x) for x in vals]
        if len(wref) != len(wgot):
            return ("WA_SHAPE_FAIL", worst, worst_ulp, n,
                    f"point {point}: {len(wref)} vs {len(wgot)}")
        if strict and not all(math.isfinite(x) for x in wref + wgot):
            return ("WA_NONFINITE", worst, worst_ulp, n,
                    f"point {point}: write_array is nonfinite")
        wworst, wulp = worst_pair(wref, wgot, floor)
        n += len(wref)
        columns = names.split(",")
        wulps = write_array_ulps(columns, wref, wgot)
        against = (WRITE_ARRAY_AGAINST_LARGEST_OFF_PLATFORM.get(model)
                   if off_platform else None)
        if against:
            wulp = max([wulps.get(f"wa {c}", 0)
                        if re.split(r"[.:]", c, maxsplit=1)[0] in against[0]
                        else pair_dev(a, b, floor)[1]
                        for c, a, b in zip(columns, wref, wgot)], default=0)
        record_ulps(measured, wulps)
        if ulp_entries is not None:
            labels = {f"wa {c}": (a, b) for c, a, b in zip(columns, wref, wgot)}
            failed = ulp_gate(ulp_entries, wulps, point, labels, held)
            if failed:
                return (failed[0], worst, max(wulps.values()), n, failed[1])
        if fast_math:
            # These outputs keep the default gate, so they are judged here
            # and kept out of the deviation the fast gate reads.
            if wworst >= WA_MAX_REL:
                return ("WA_GATE", worst, worst_ulp, n,
                        f"point {point}: write_array {wworst:.2e}, "
                        f"allowed {WA_MAX_REL:.1e}")
        else:
            wwhere = worst_values(wref, wgot, floor, {},
                                  lambda i: column_label(columns[i], opts))
            where = tuple(b if dev > a_dev else a for a, b, a_dev, dev in
                          zip(where, wwhere, (worst, worst_ulp), (wworst, wulp)))
            worst, worst_ulp = max(worst, wworst), max(worst_ulp, wulp)
    return ("OK", worst, worst_ulp, n, where)


def density_label(index, opts):
    if index == 0:
        return "lp"
    labels = opts.grad_labels if opts else ()
    return f"grad[{index}]" + (f" {labels[index - 1]}" if index <= len(labels) else "")


def column_label(name, opts):
    case = opts.column_labels.get(name) if opts else None
    return name + (f" {case}" if case else "")


def worst_values(rv, gv, floor, skip, label):
    """The values with the worst scaled error and worst ULP, each named
    and shown on both sides, so a failure on a platform nobody can run
    locally carries the numbers needed to diagnose it."""
    devs = [(pair_dev(a, b, floor), i) for i, (a, b) in enumerate(zip(rv, gv))
            if i not in skip]
    if not devs:
        return ("", "")
    rel = max(devs, key=lambda d: d[0][0])[1]
    ulp = max(devs, key=lambda d: d[0][1])[1]
    show = lambda i: f"{label(i)} (stanli {gv[i]!r}, CmdStan {rv[i]!r})"
    return (show(rel), show(ulp))


def check_model(model, ref, pdb, check_bin, tmp, timeout, max_rel,
                no_wa=False, target_platform=None,
                fast_math=False, opts=None, ulp_entries=None, measured=None,
                truth=None, truth_measured=None):
    """Replay one model, under whatever KNOWN_GAPS says about it.

    A WRITE_ARRAY gap replays lp and the gradient first, without the
    write_array, and any failure there is the model's result.
    """
    args = (model, ref, pdb, check_bin, tmp, timeout, max_rel)
    rest = (target_platform, fast_math, opts, ulp_entries, measured)
    held = {"truth": truth, "truth_measured": truth_measured}
    gap = KNOWN_GAPS.get(model)
    density = None
    if gap is not None and gap.excuses == WRITE_ARRAY:
        density = replay_model(*args, True, *rest, wa_gap=True, **held)
        if density[1] != "OK":
            return density
    result = replay_model(*args, no_wa, *rest, **held)
    if gap is None:
        return result
    _, status, rel, ulp, total, _, notes = result
    notes.append(f"KNOWN GAP {model}: {gap.reason}")
    if status == "CRASH":
        return result
    if status == "OK":
        return (model, "GAP_CLOSED", rel, ulp, total,
                f"{model} matches its references now "
                f"({gap.reason}); delete it from KNOWN_GAPS", notes)
    if density is not None:
        return density[:6] + (density[6] + notes,)
    return (model, "OK", 0.0, 0, total, "", notes)


def replay_model(model, ref, pdb, check_bin, tmp, timeout, max_rel,
                 no_wa=False, target_platform=None,
                 fast_math=False, opts=None, ulp_entries=None, measured=None,
                 wa_gap=False, truth=None, truth_measured=None):
    """Replay every point of one model.

    ulp_entries, measured, wa_gap, truth and truth_measured are as in
    check_point, over all points.

    Returns (model, status, max_rel, max_ulp, n_values, detail, notes).
    The deviation reported is the worst over the values held to the clean
    gate. The values that floored_outputs names are gated against their
    own floor (gate_for) and kept out of that number, so one documented
    deviation cannot become the corpus-wide "worst".

    The points run sequentially inside one pool task rather than as three
    tasks, because model_files unpacks the shared posteriordb data zip to
    one path per model and three workers writing it at once would race.
    A local corpus (opts) has its files already and runs them in parallel.
    """
    stan, dj = (opts.source, opts.data) if opts else model_files(model, ref,
                                                                 pdb, tmp)
    if not stan.exists() or not dj.exists():
        return (model, "MISSING_INPUT", 0.0, 0, 0, str(stan), [])
    for key, path in (("source_sha256", stan), ("data_sha256", dj)):
        if key in ref and ref[key] != source_digest(path):
            return (model, "INPUT_HASH_FAIL", 0.0, 0, 0, key, [])
    if model in ULP_LIMITS and not ref.get("recorded", {}).get("platform"):
        return (model, "REFERENCE_INCOMPLETE", 0.0, 0, 0,
                "ULP-gated fixtures require recording platform provenance", [])
    if (ref.get("seed", 1234), ref.get("chain", 0)) != (1234, 0):
        return (model, "REFERENCE_INCOMPLETE", 0.0, 0, 0,
                "recorded with another RNG seed or chain than stanli_check's",
                [])
    # Fast mode is not held to the same-platform ULP limits.
    if opts and opts.ulp is not None:
        ulp_limit = opts.ulp
    else:
        ulp_limit = None if fast_math else ulp_limit_for(
            model, ref, target_platform or native_platform())
    recorded_on = ref.get("recorded", {}).get("platform")
    off_platform = recorded_on is not None and recorded_on != (
        target_platform or native_platform())
    strict = ref.get("strict", False) or bool(opts and opts.strict)
    if strict:
        if set(ref.get("points", {})) != {str(p) for p in POINTS}:
            return (model, "REFERENCE_INCOMPLETE", 0.0, 0, 0,
                    "all deterministic reference points are required", [])
        for pt in ref["points"].values():
            if "values" not in pt or "wa" not in pt:
                return (model, "REFERENCE_INCOMPLETE", 0.0, 0, 0,
                        "lp/gradient and write_array references are required", [])
    worst, worst_ulp, total, notes = 0.0, 0, 0, []
    if measured is None:
        measured = {}
    if truth_measured is None:
        truth_measured = {}
    unnamed = [p for p, pt in ref["points"].items()
               if pt.get("status") == "MISMATCH" and model not in MISMATCH_OUTPUTS]
    if unnamed:
        return (model, "REFERENCE_INCOMPLETE", 0.0, 0, 0,
                f"points {', '.join(unnamed)} are recorded MISMATCH; name the "
                f"values that disagree in MISMATCH_OUTPUTS", [])
    floored = {int(p): floored_outputs(model, pt, max_rel)
               for p, pt in ref["points"].items()}
    done = {}
    if opts:
        # One dict per point: the workers would race on a shared one.
        point_measured = {point: {} for point in POINTS}
        point_truth = {point: {} for point in POINTS}
        with concurrent.futures.ThreadPoolExecutor(len(POINTS)) as pool:
            done = {point: pool.submit(
                        check_point, model, stan, dj, check_bin, point,
                        ref["points"][str(point)], timeout, no_wa, strict,
                        fast_math, opts, off_platform, ulp_entries,
                        point_measured[point], floored[point], notes, wa_gap,
                        truth, point_truth[point])
                    for point in POINTS if str(point) in ref["points"]
                    and (model, point) not in QUARANTINED}
    for point in POINTS:
        pt = ref["points"].get(str(point))
        if (model, point) in QUARANTINED:
            # The reference is recorded and is not enforced: stanli is
            # known to disagree here. Say so on every run -- a quarantine
            # nobody sees is an exception that outlives its bug -- and
            # hold the point to what a point with no reference is held to,
            # so a crash here still fails.
            notes.append(f"QUARANTINE {model} point {point}: "
                         f"{QUARANTINED[(model, point)]}")
            status, detail = probe_point(model, stan, dj, check_bin, point,
                                         timeout, fast_math, opts)
            if status != "OK":
                return (model, status, worst, worst_ulp, total,
                        f"point {point}: {detail}", notes)
            continue
        if pt is None:
            status, detail = probe_point(model, stan, dj, check_bin, point,
                                         timeout, fast_math, opts)
            if status != "OK":
                return (model, status, worst, worst_ulp, total,
                        f"point {point}: {detail}", notes)
            notes.append(f"UNREFERENCED {model} point {point}: ran clean, "
                         f"but nothing compared it against CmdStan")
            continue
        if point in done:
            status, rel, ulp, n, detail = done[point].result()
            for label, got in point_measured[point].items():
                measured[label] = max(measured.get(label, 0), got)
            for label, got in point_truth[point].items():
                truth_measured[label] = max(truth_measured.get(label, 0.0), got)
        else:
            status, rel, ulp, n, detail = check_point(
                model, stan, dj, check_bin, point, pt, timeout, no_wa, strict,
                fast_math, off_platform=off_platform, ulp_entries=ulp_entries,
                measured=measured, floored=floored[point], notes=notes,
                wa_gap=wa_gap, truth=truth, truth_measured=truth_measured)
        total += n
        if status != "OK":
            return (model, status, worst, worst_ulp, total, detail, notes)
        # The values of a documented model that keep the clean gate get
        # the default-mode one in fast mode too.
        gate = (max(max_rel, WA_MAX_REL) if fast_math and floored[point]
                else max_rel)
        if fast_math and model in FAST_HP_CHECKED:
            gate = max(gate, FAST_HP_CHECKED[model][0])
        if rel >= gate:
            return (model, "GATE", rel, ulp, total,
                    f"point {point}: {rel:.2e} ({ulp} ulp) over {n} "
                    f"values, allowed {gate:.1e}, worst at {detail[0]}", notes)
        if ulp_limit is not None and ulp > ulp_limit:
            return (model, "ULP_GATE", rel, ulp, total,
                    f"point {point}: {ulp} ULP over {n} values, "
                    f"allowed {ulp_limit}, worst at {detail[1]}", notes)
        if floored[point] is None:
            worst, worst_ulp = max(worst, rel), max(worst_ulp, ulp)
    if ulp_entries or truth:
        notes += ledger_notes(model, ulp_entries or {}, measured, truth,
                              truth_measured)
    return (model, "OK", worst, worst_ulp, total, "", notes)


def short_wa(msg):
    match = re.search(r"unsupported (?:function |statement function )?([\w]+)",
                      msg)
    return (f"unsupported {match[1]}" if match
            else msg.split("|")[0].strip()[:80])


def wa_coverage(status, point):
    """(row status, detail, reason or None) for one model's write_array.

    The reason is what stops the section, for the report's tally.
    """
    section = status.write_array()
    if section["section"] == "none":
        return "NO_GQ_SECTION", "", None
    truncated = section.get("truncated")
    wa = status.points.get(point, {}).get("write_array")
    if wa is None:
        return "EMPTY", truncated or "", truncated or ""
    if not wa["ok"]:
        return "EMPTY", wa["message"], wa["message"]
    note = (f", {wa['nonfinite']}/{wa['values']} nonfinite"
            if wa["nonfinite"] else "")
    if section["section"] == "interpreted":
        return "COMPLETE", f"{wa['columns']} vars (interpreted){note}", None
    if truncated is not None:
        return "TRUNCATED", truncated + note, truncated
    return "COMPLETE", f"{wa['columns']} vars{note}", None


def check_wa_coverage(pdb, check_bin, models, contains, timeout, excluded=()):
    """Probe write_array at points 0--2 and report corpus coverage."""
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_wa_"))
    rows, reasons = [], collections.Counter()
    selected = list(corpus_models(pdb, models, contains, excluded))
    if not selected:
        print("no corpus models selected", file=sys.stderr)
        return 2
    runnable = 0
    for model, data_name in selected:
        inputs = corpus_input(pdb, tmp, model, data_name)
        if inputs is None:
            continue
        runnable += 1
        stan, data = inputs
        try:
            for point in (0, 1, 2):
                proc, status = run_check([check_bin, stan, data, "--point",
                                          point], timeout)
                if status.points.get(point, {}).get("ok"):
                    break
        except subprocess.TimeoutExpired:
            rows.append((model, "TIMEOUT", ""))
            continue
        if not status.points.get(point, {}).get("ok"):
            continue
        name, detail, reason = wa_coverage(status, point)
        rows.append((model, name, detail))
        if reason is not None:
            reasons[short_wa(reason)] += 1
    counts = collections.Counter(row[1] for row in rows)
    print(f"{len(rows)} compiling models")
    for name in ("COMPLETE", "TRUNCATED", "EMPTY", "NO_GQ_SECTION",
                 "TIMEOUT"):
        if counts[name]:
            print(f"  {counts[name]:4d}  {name}")
    if reasons:
        print("\nwhat stops the rest:")
        for reason, count in reasons.most_common(30):
            print(f"  {count:4d}  {reason}")
    print("\nper model:")
    for model, status, detail in sorted(rows, key=lambda row: (row[1], row[0])):
        print(f"  {status:14s} {model:44s} {detail[:110]}")
    if runnable == 0 or not rows:
        print("no selected model was checked", file=sys.stderr)
        return 2
    return 0


def gate_for(model, pt, default):
    """The threshold for the values of this recorded point that
    floored_outputs names.

    A point recorded as MISMATCH is one whose disagreement with CmdStan is
    documented and understood (kronecker_gp: two of 438 gradients flow
    through eigenvectors of a nearly degenerate covariance). Gating those
    values at the clean threshold would fail every run; ignoring them would
    let a real regression hide behind a known deviation. Gate them above
    what they were recorded at, so they can never get much worse unnoticed.

    A point recorded with no finite deviation at all -- one side nonfinite
    where the other is not, which pair_dev scores as infinite -- gets the
    clean gate and therefore fails. There is no threshold above infinity,
    and deriving one from the recording would turn "stanli was wrong here
    when this was recorded" into "stanli may be wrong here forever". Such
    a point passes only by being fixed, or by an entry in QUARANTINED that
    someone has to write down and delete.

    4x, not 2x: an ill-conditioned eigendecomposition amplifies ISA-level
    differences, and the deviation itself moves across platforms. Measured
    for kronecker_gp point 0: 7.1e-3 on arm64 (where the reference was
    recorded), 1.71e-2 on both x86_64 runners, identical to each other.

    And the model's Mismatch.floor, where the amplification is not
    proportional to the deviation: kronecker_gp's point 2 recorded 1.05e-3
    on arm64 and measured 8.2e-3 on x86_64 -- 7.8x where point 0 moved
    2.4x -- so a pure multiplier under-gates precisely its smallest
    recorded deviations. An ILL_CONDITIONED model's gradient gets
    ILL_CONDITIONED_GATE instead.
    """
    if model in ILL_CONDITIONED:
        return max(ILL_CONDITIONED_GATE, default)
    rel = pt.get("max_rel")
    if rel is None or pt.get("status") != "MISMATCH":
        return default
    return max(rel * 4.0, MISMATCH_OUTPUTS[model].floor, default)

# The corpora CTest replays: each is a set of models, one reference archive
# in the corpus schema, and how to run and gate them. Unlike the corpus
# above they need no posteriordb checkout.
FUNCTIONS = REPO / "tests" / "function_coverage"
FIXTURES = REPO / "tests" / "fixtures"
EMPTY_DATA = FUNCTIONS / "empty.json"
SIGNATURE_MANIFESTS = (FUNCTIONS / "builtin_signatures_manifest.json",
                       FUNCTIONS / "density_signatures_manifest.json")
# A signature model's case comments, one per case block in order. Case k
# reads parameter k and writes generated_quantities_result.k.
SIGNATURE_CASE = re.compile(r"// (\S+\(\S*=>\S+)$", re.MULTILINE)

# Fixtures whose replay must run through the MIR interpreter. The
# *_fallback twins hold the interpreter path to CmdStan on purpose.
INTERPRETED = frozenset({
    "gq_callback_integer_contexts_fallback",
    "matrix_callback_contexts_fallback", "matrix_callback_geometry_edges",
    "matrix_callback_value_fallback", "nested_callback_contexts_fallback",
    "ode_matrix_callback_fallback", "ode_nested_callback_fallback"})


def nested_rate_gradient(outputs):
    gate, rate = outputs[:2]
    return 3.96 * (2 if gate > 0 else 1) - rate


# Values held to their own ULP limit: {model: {value index: (limit, cause,
# independent value computed from the outputs, which must agree to 2 ULP)}}.
VALUE_ULP_LIMITS = {
    name: {2: (16, "the adjoint ODE sensitivity in the combined rate "
                   "gradient differs by 14-16 ULP; both callback engines "
                   "agree", nested_rate_gradient)}
    for name in ("nested_callback_contexts",
                 "nested_callback_contexts_fallback")}


# Fixtures whose gradient is measured against its largest entry when replayed
# on a platform other than the one the reference was recorded on. Their
# covariance comes from Eigen's matrix product, whose arm64 kernel fuses each
# multiply-add; x86-64 and MSVC builds round the product first. Gradient
# entries that cancel to a hundredth of the largest one amplify that last
# bit: stanli on Linux and Windows is up to 507 ULP per element from the
# arm64 CmdStan reference, and 4 ULP of the largest entry. Against an
# 80-digit reference the Linux values are farther from the true gradient at
# five of the six largest gaps (propto_multi_gp grad[95]: 166 vs 70 ULP) and
# closer at one (multi_gp_rectangular grad[19]: 222 vs 285). CmdStan built on
# arm64 with Eigen's kernel changed to round each product gives the same value
# as stanli on Linux at each of those entries and is within 2 ULP elsewhere,
# and the largest gradient error relative to the largest entry is the same on
# both platforms.
GRADIENT_AGAINST_LARGEST_OFF_PLATFORM = dict.fromkeys(
    ("multi_gp_rectangular", "propto_data_arguments", "propto_multi_gp"),
    "cancelling covariance gradient; Eigen's product fuses on arm64 only")

# write_array variables that are measured in ULP of the variable's largest
# CmdStan value, as the ULP ledger measures them, when replayed on a platform
# other than the one the reference was recorded on. lkj_corr_rng draws each
# correlation as 2 * beta - 1, and Boost's gamma sampler under beta_rng calls
# tan. At one draw of this fixture macOS's tan is 0.97 ULP from the true
# value, while Linux and Windows return the correctly rounded value. The
# subtraction of 1 turns the 2 ULP that the beta draw then moves into 16 ULP
# of a correlation near 0.25. Stan Math with a correctly rounded tan, run on macOS,
# gives the Linux value exactly. Its lk values are then at most 3 ULP from
# the macOS reference, counted in ULP of 1, the largest entry, and no other
# output moves.
WRITE_ARRAY_AGAINST_LARGEST_OFF_PLATFORM = {
    "gq_library_rng": (("lk",), "lkj_corr_rng correlations depend on the "
                                "platform's tan"),
}


@dataclasses.dataclass(frozen=True, eq=False)
class Replay:
    """How check_point runs and gates one model of a local corpus."""
    source: pathlib.Path
    data: pathlib.Path
    args: tuple = ()
    interpreted: bool = False
    ulp: object = None
    scale_floor: float = 1.0
    strict: bool = False
    grad_labels: tuple = ()
    column_labels: dict = dataclasses.field(default_factory=dict)
    value_limits: dict = dataclasses.field(default_factory=dict)


@dataclasses.dataclass(frozen=True)
class Corpus:
    refs: pathlib.Path
    # A function of a work directory giving name -> (source, data, gradient
    # labels[, manifest source hash]); None takes the models from the archive.
    models: object
    mir: bool = False
    ulp: object = None
    scale_floor: float = 1.0
    strict: bool = True
    case_labels: bool = False
    # A function of a model's reference giving the reference to replay
    # against, or None for the recorded one.
    reference: object = None
    # Recorded before references carried provenance; nothing ties them to
    # a pin until they are re-recorded.
    unpinned: bool = False


def own_data(stan, suffix=".json"):
    data = stan.with_name(stan.stem + suffix)
    return data if data.exists() else EMPTY_DATA


def manifest_models(manifest, directory):
    def models(_work):
        listed = json.loads(manifest.read_text())["models"]
        return {name: (directory / f"{name}.stan",
                       own_data(directory / f"{name}.stan"),
                       tuple(entry.get("gradient", ())))
                for name, entry in listed.items()}
    return models


def signature_models(work):
    """The generated signature models, checked against their manifests."""
    sys.path.insert(0, str(REPO / "harnesses"))
    from function_signature_common import CONTEXT_DATA
    # Concurrent replays each write their own seed: Windows refuses to
    # replace a file another process has open.
    seed = work / "context_seed.json"
    seed.write_text(json.dumps(CONTEXT_DATA) + "\n")
    models = {}
    for manifest in SIGNATURE_MANIFESTS:
        for model in json.loads(manifest.read_text())["models"]:
            source = REPO / model["file"]
            models[source.stem] = (source, seed, (), model["source_sha256"])
    return models


def signature_exact_lp(ref):
    """ref with each point's log density replaced by the exact value of the
    expression that a signature model adds, rounded once.

    A signature model's lp is std_normal on the probe plus twice the sum of
    its cases, -0.5 * sum(probe^2) + 2 * sum(t), and the recording keeps the
    probe and every case value t in its write_array row. CmdStan adds the 360
    to 1,440 case values in order, so its recorded lp carries the rounding of
    that sum; the exact value of the same expression from the same recorded
    doubles does not. A point without a row, or with a nonfinite case, keeps
    CmdStan's lp."""
    points = {}
    for key, point in ref["points"].items():
        wa = point.get("wa")
        values = point.get("values")
        if wa and values:
            row = dict(zip(wa["names"].split(","), map(float, wa["values"])))
            probe = [x for n, x in row.items() if n.startswith("probe.")]
            cases = [x for n, x in row.items()
                     if n.startswith("generated_quantities_result.")]
            if all(math.isfinite(x) for x in probe + cases):
                exact = (-Fraction(1, 2) * sum(Fraction(x) ** 2 for x in probe)
                         + 2 * sum(map(Fraction, cases)))
                point = dict(point, values=[float(exact)] + list(values[1:]))
        points[key] = point
    return dict(ref, points=points)


def fixture_models(names):
    def models(_work):
        return {name: (FIXTURES / f"{name}.stan",
                       own_data(FIXTURES / f"{name}.stan"), ())
                for name in names}
    return models


def replay_models(_work):
    found = {}
    for stan in sorted((FIXTURES / "cmdstan_replay").glob("*.stan")):
        found[stan.stem] = (stan, own_data(stan, ".data.json"), ())
    stan = FIXTURES / "viewc_zero_length_whole_assignment_resize.stan"
    found[stan.stem] = (stan, EMPTY_DATA, ())
    return found


def collection_models(collection):
    def models(_work):
        return {name: (case.source, case.data, ())
                for name, case in local_cases().items()
                if case.collection == collection}
    return models


CORPORA = {
    "functions": Corpus(FUNCTIONS / "references.json.gz",
                        manifest_models(FUNCTIONS / "manifest.json",
                                        FUNCTIONS)),
    # Library functions at sizes the unit-extent signature models cannot
    # reach: ordering and ties, rectangular algebra, zero extents, GLMs.
    "sized_functions": Corpus(FUNCTIONS / "sized_functions_references.json.gz",
                              None),
    "container_flow": Corpus(
        FUNCTIONS / "container_flow_references.json.gz",
        manifest_models(FUNCTIONS / "container_flow_manifest.json",
                        FUNCTIONS / "container_flow")),
    "signatures": Corpus(FUNCTIONS / "signature_references.json.gz",
                         signature_models, case_labels=True,
                         reference=signature_exact_lp),
    "fixtures": Corpus(FIXTURES / "references.json.gz", None, mir=True,
                       ulp=10),
    # Lowering rewrites that only inlined O1 MIR reaches, so these also run
    # with --fixture-passes. isclose(rel 1e-9, abs 1e-12): the scaled
    # error with values below 1e-3 held to an absolute 1e-12.
    "cmdstan_replay": Corpus(FIXTURES / "cmdstan_replay" / "references.json.gz",
                             replay_models, scale_floor=1e-3, strict=False,
                             unpinned=True),
    "oncobayes2": Corpus(REFS_PATH, collection_models("oncobayes2"),
                         strict=False),
}


def load_corpus_refs(corpus):
    models, recorded = load_refs(corpus.refs)
    return ({model: {**ref, "recorded": ref.get("recorded", recorded)}
             for model, ref in models.items()}, recorded)


PINS = (("cmdstan", "CMDSTAN_SHA"), ("stan", "STAN_SHA"),
        ("math", "MATH_SHA"), ("stanc3", "STANC3_UPSTREAM_SHA"))


def pin_drift(recorded, required=("cmdstan", "stan", "math")):
    """Why a recording no longer matches deps/pins.env, or "".

    A reference describes one CmdStan build; after a pin bump it must be
    re-recorded rather than replayed against a toolchain nobody pins.
    """
    pins = toolchain.pins()
    for key, pin in PINS:
        if key not in recorded:
            if key in required:
                return f"recorded without its {key} revision"
        elif recorded[key] != pins[pin]:
            return (f"recorded against {key} {recorded[key][:12]}, "
                    f"deps/pins.env pins {pins[pin][:12]}; re-record it")
    return ""


def replay_corpus(name, selected, check_bin, passes, jobs, timeout,
                  target_platform=None, update_ledger=False, first_reasons=None):
    """Replay a local corpus against its archive. Exit status.

    first_reasons is the --add-ulp-entries file's content, or None.
    """
    corpus = CORPORA[name]
    refs, _ = load_corpus_refs(corpus)
    target_platform = target_platform or native_platform()
    ledger_key = name + (" --fixture-passes" if passes else "")
    ledger = load_ulp_ledger()
    section = ulp_section(ledger, target_platform, ledger_key)
    measured = collections.defaultdict(dict)
    with tempfile.TemporaryDirectory(prefix=f"stanli_{name}_") as tmp:
        work = pathlib.Path(tmp)
        models = (corpus.models(work) if corpus.models
                  else fixture_models(sorted(refs))(work))
        missing = sorted(set(models) - set(refs))
        if missing:
            print("no reference recorded for: " + " ".join(missing))
            return 2
        if corpus.refs != REFS_PATH and set(refs) != set(models):
            print("references for models the corpus no longer has: "
                  + " ".join(sorted(set(refs) - set(models))))
            return 2
        unknown = sorted(set(selected) - set(models))
        if unknown:
            print(f"not in the {name} corpus: " + " ".join(unknown))
            return 2
        if passes and corpus.mir:
            print(f"the {name} corpus replays its build-generated MIR")
            return 2
        if section is not None and set(section) - set(models):
            print("ULP ledger entries for models the corpus no longer has: "
                  + " ".join(sorted(set(section) - set(models))))
            return 2
        if first_reasons is not None:
            problems = first_entry_models(first_reasons, section,
                                          selected or sorted(models))
            if problems:
                print("\n".join(problems))
                return 2
        print(ulp_gate_banner(section, target_platform, ledger_key))
        failures, values = [], 0

        def one(model):
            source, data, labels, *manifest = models[model]
            ref = refs[model]
            if corpus.reference:
                ref = corpus.reference(ref)
            drift = pin_drift(ref["recorded"], () if corpus.unpinned else
                              ("cmdstan", "stan", "math"))
            if drift or "data_sha256" not in ref:
                return (model, "REFERENCE_INCOMPLETE", 0.0, 0, 0,
                        drift or "no data_sha256", [])
            if manifest and source.exists() and source_digest(source) != manifest[0]:
                return (model, "STALE_SOURCE", 0.0, 0, 0,
                        f"{source} disagrees with its manifest; rerun the "
                        "signature generators", [])
            if not source.exists():
                return (model, "MISSING_INPUT", 0.0, 0, 0,
                        f"{source} is not generated yet; build the "
                        "stanli_signature_models target", [])
            args = ()
            if corpus.mir:
                args = ("--mir", str(source.with_suffix(".mir")))
            elif passes:
                args = ("--passes", passes)
            columns = {}
            if corpus.case_labels:
                labels = tuple(SIGNATURE_CASE.findall(source.read_text()))
                columns = {f"generated_quantities_result.{k}": case
                           for k, case in enumerate(labels, 1)}
            opts = Replay(source, data, args, model in INTERPRETED,
                          corpus.ulp, corpus.scale_floor, corpus.strict,
                          labels, columns, VALUE_ULP_LIMITS.get(model, {}))
            return check_model(model, ref, None, check_bin, work, timeout,
                               1e-9, target_platform=target_platform,
                               opts=opts,
                               ulp_entries=(None if section is None
                                            or model in (first_reasons or {})
                                            else section.get(model, {})),
                               measured=measured[model])

        leg = " with the fixture passes" if passes else ""
        with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
            for model, status, rel, ulp, n, detail, notes in pool.map(
                    one, selected or sorted(models)):
                for note in notes:
                    print(note)
                values += n
                if status != "OK":
                    failures.append(model)
                    print(f"{status} {model}{leg} {detail}")
                else:
                    print(f"PASS {model}{leg}: {n} values, worst {rel:.2e} "
                          f"({ulp} ulp)")
    count = len(selected or models)
    print(f"\n{count - len(failures)}/{count} {name} models match their "
          f"CmdStan references{leg} ({values} values compared)")
    if failures:
        print(f"{len(failures)} FAILED: " + " ".join(failures))
        if update_ledger or first_reasons is not None:
            print("ULP ledger not updated: every model must pass first")
        return 1
    if update_ledger:
        write_ulp_ledger(ledger, target_platform, ledger_key, measured,
                         selected or sorted(models),
                         {m: refs[m]["recorded"].get("platform") for m in refs})
        print(f"updated {ULP_LEDGER_PATH.relative_to(REPO)} for "
              f"{target_platform}, {ledger_key}")
    if first_reasons is not None:
        return report_first_entries(ledger, target_platform, ledger_key,
                                    measured, first_reasons)
    return 0


def report_first_entries(ledger, target_platform, corpus, measured, reasons):
    problems = add_first_entries(ledger, target_platform, corpus, measured,
                                 reasons)
    if problems:
        print("\n".join(problems))
        print("ULP ledger not updated: give every value over "
              f"{DEFAULT_ULP_BOUND} a reason, and no other")
        return 1
    print(f"wrote the entries of {len(reasons)} models to "
          f"{ULP_LEDGER_PATH.relative_to(REPO)} for {target_platform}, {corpus}")
    return 0


def ulp_gate_banner(section, target_platform, corpus):
    if section is None:
        return (f"ULP gate off: {ULP_LEDGER_PATH.name} has no {corpus} section "
                f"for {target_platform}")
    return (f"ULP gate: every value within {DEFAULT_ULP_BOUND} scaled ULP of "
            f"CmdStan, {len(section)} models with measured exceptions on "
            f"{target_platform}")


def truth_gate_banner(truth):
    entries = sum(len(entries) for entries in truth.values())
    return (f"true-value gate: {entries} ledger entries of {len(truth)} models "
            f"within max(bound, {DEFAULT_ULP_BOUND}) scaled ULP of their "
            f"80-digit values, on every platform")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pdb", type=pathlib.Path, nargs="?")
    ap.add_argument("models", nargs="*")
    ap.add_argument("--corpus", choices=sorted(CORPORA),
                    help="replay a local corpus instead of the posteriordb one")
    ap.add_argument("--model", action="append", default=[],
                    help="with --corpus, replay only this model")
    ap.add_argument("--fixture-passes", action="store_true",
                    help="with --corpus, compile with the MIR fixtures' O1 "
                         "passes, which inline functions")
    ap.add_argument("--check", type=pathlib.Path,
                    default=default_check_bin())
    ap.add_argument("--max-rel", type=float, default=None,
                    help="gate on lp and gradients (default 1e-9, or "
                         f"{FAST_MAX_REL:g} with --fast-math)")
    ap.add_argument("--fast-math", action="store_true",
                    help="replay with stanli_check --fast-math under the "
                         "fast-mode gate (TESTING.md): the gradient is "
                         "scored against its largest entry and the "
                         "same-platform ULP limits do not apply")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--target-platform", default=native_platform(),
                    help="runtime platform for reference selection (default: "
                         "native host); use 'WebAssembly wasm32' for the Node driver")
    ap.add_argument("--reference-compiler", choices=("clang", "gcc", "msvc", "unknown"),
                    help="external driver's C++ compiler; normally read from stanli_check")
    ap.add_argument("--no-wa", action="store_true",
                    help="replay lp and gradients only (the WASM check "
                         "driver has no write_array entry point yet)")
    ap.add_argument("--wa-report", action="store_true",
                    help="report structural write_array coverage")
    ap.add_argument("--wa-headers", type=pathlib.Path, metavar="CMDSTAN_DIR",
                    help="compare write_array headers with live CmdStan")
    ap.add_argument("--skip", default="",
                    help="comma-separated models to exclude (the wasm32 "
                         "build cannot fit nn_rbm1bJ100's compile in 4GB)")
    ap.add_argument("--timeout", type=float, default=300)
    ap.add_argument("--per-model", action="store_true",
                    help="print each model's worst deviation and ULP")
    ap.add_argument("--filter", default="", metavar="SUBSTR",
                    help="with --wa-report, select model names containing this")
    ap.add_argument("--update-ulp-ledger", action="store_true",
                    help="after a passing replay, write the measured ULP "
                         "exceptions for this platform into "
                         f"{ULP_LEDGER_PATH.relative_to(REPO)}; existing "
                         "bounds only go down")
    ap.add_argument("--add-ulp-entries", type=pathlib.Path, metavar="REASONS",
                    help="after a passing replay, give the models in REASONS, "
                         "a JSON file of {model: {label: reason}}, new "
                         "ledger entries; a reason is a string or "
                         '{"reason": ..., "closer": "stanli", "cmdstan" or '
                         '"unknown"}, and a string means "unknown"; each '
                         "value over "
                         f"{DEFAULT_ULP_BOUND} needs a reason, or an "
                         "existing entry that it is within")
    args = ap.parse_intermixed_args()
    if (args.update_ulp_ledger or args.add_ulp_entries) and (
            args.fast_math or args.no_wa):
        ap.error("--update-ulp-ledger and --add-ulp-entries measure the "
                 "default build with write_array")
    if args.update_ulp_ledger and args.add_ulp_entries:
        ap.error("--update-ulp-ledger and --add-ulp-entries are separate runs")
    first_reasons = (load_first_entry_reasons(args.add_ulp_entries)
                     if args.add_ulp_entries else None)
    if args.corpus:
        if args.pdb or args.models:
            ap.error("--corpus takes models with --model")
        return replay_corpus(args.corpus, args.model, args.check.resolve(),
                             gen_fixtures.O1_PASSES if args.fixture_passes
                             else None, args.jobs,
                             args.timeout, args.target_platform,
                             args.update_ulp_ledger, first_reasons)
    if args.pdb is None:
        ap.error("the posteriordb checkout is required without --corpus")
    if args.max_rel is None:
        args.max_rel = FAST_MAX_REL if args.fast_math else 1e-9

    pdb = args.pdb / "posterior_database"
    if args.wa_headers and args.wa_report:
        ap.error("--wa-headers and --wa-report are separate modes")
    if args.filter and not args.wa_report:
        ap.error("--filter requires --wa-report")
    check_bin = args.check
    skip = set(filter(None, args.skip.split(",")))
    if args.models and all(model in skip for model in args.models):
        ap.error("--skip excludes every requested model")
    if args.wa_headers:
        try:
            toolchain.require_pinned_toolchain(args.wa_headers.resolve())
        except ValueError as exc:
            ap.error(str(exc))
        return check_wa_headers(pdb, check_bin, args.wa_headers.resolve(),
                                args.models, skip)
    if args.wa_report:
        return check_wa_coverage(pdb, check_bin, args.models, args.filter,
                                 args.timeout, skip)
    compiler = args.reference_compiler
    if compiler is None and (args.target_platform in PLATFORM_REFS or
                             args.target_platform in SPARSE_PLATFORM_REFS):
        compiler = runtime_compiler(check_bin)
    refs, recorded = replay_refs(args.target_platform, compiler)
    # All local inventory additions must enter the default gate; iterating
    # existing references alone silently omits fixtures that failed to record.
    cases = corpus_cases(pdb, include_language=True)
    local = {name for name, case in cases.items() if case.collection != "posteriordb"}
    models = args.models or sorted(set(refs) | local)
    models = [m for m in models if m not in skip]
    missing = [m for m in models if m not in refs]
    if missing:
        print(f"no reference recorded for: {' '.join(missing)}")
        return 2
    drifted = [(m, pin_drift(refs[m]["recorded"])) for m in models]
    drifted = [(m, why) for m, why in drifted if why]
    if drifted:
        for model, why in drifted:
            print(f"REFERENCE_INCOMPLETE {model} {why}")
        return 2

    # Which CmdStan these values came from is part of reading the result:
    # the gate is 1e-9 against one specific build of one specific pin.
    print(f"references recorded against CmdStan "
          f"{recorded['cmdstan_version']} ({recorded['cmdstan'][:12]}), "
          f"math {recorded['math'][:12]}, on {recorded['platform']}")
    overrides = sum(refs[m]["recorded"] != recorded for m in models)
    if overrides:
        print(f"{overrides} models retain per-model recording provenance overrides")
    if args.fast_math:
        print(f"fast mode: lp and gradient (against its largest entry) "
              f"within {args.max_rel:.0e}; other outputs within "
              f"{WA_MAX_REL:.0e}; no same-platform ULP limits")
    else:
        tight = sum(ulp_limit_for(m, refs[m], args.target_platform) is not None
                    for m in models)
        print(f"same-platform ULP limits: {tight} models on "
              f"{args.target_platform}; all models retain their "
              "scaled-error and structural checks")
    if compiler:
        print(f"runtime compiler: {compiler}; references selected before evaluation")
    # Fast mode has its own gate, and the wasm driver has no write_array.
    ledger = load_ulp_ledger()
    section = (None if args.fast_math or args.no_wa
               else ulp_section(ledger, args.target_platform, "main"))
    if section is not None and not args.models and set(section) - set(refs):
        print("ULP ledger entries for models with no reference: "
              + " ".join(sorted(set(section) - set(refs))))
        return 2
    if first_reasons is not None:
        problems = first_entry_models(first_reasons, section, models)
        if problems:
            print("\n".join(problems))
            return 2
    print("ULP gate off: fast mode and --no-wa replays keep their own gates"
          if args.fast_math or args.no_wa
          else ulp_gate_banner(section, args.target_platform, "main"))
    # The true values do not depend on the platform, so they hold on every
    # platform, also where the ledger has no section.
    truth = ({} if args.fast_math or args.no_wa
             else truth_section(ledger, "main"))
    if not args.models and set(truth) - set(refs):
        print("true values for models with no reference: "
              + " ".join(sorted(set(truth) - set(refs))))
        return 2
    if truth:
        print(truth_gate_banner(truth))
    measured = {m: {} for m in models}
    truth_measured = {m: {} for m in models}
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="stanli_refs_"))
    failures, values = [], 0
    worst_overall = ("", 0.0, 0)
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futs = [pool.submit(check_model, m, refs[m], pdb, check_bin, tmp,
                            args.timeout, args.max_rel, args.no_wa,
                            args.target_platform, args.fast_math, None,
                            None if section is None or m in (first_reasons or {})
                            else section.get(m, {}),
                            measured[m], truth.get(m), truth_measured[m])
                for m in models]
        for fut in concurrent.futures.as_completed(futs):
            model, status, rel, ulp, n, detail, notes = fut.result()
            for note in notes:
                print(note)
            values += n
            if status != "OK":
                failures.append((model, status, detail))
                print(f"{status} {model} {detail}")
                continue
            if args.per_model:
                print(f"{model}\t{rel:.2e}\t{ulp}")
            if rel > worst_overall[1]:
                worst_overall = (model, rel, ulp)

    ok = len(models) - len(failures)
    print(f"\n{ok}/{len(models)} models within {args.max_rel:.0e} of the "
          f"CmdStan references at all {len(POINTS)} evaluation points "
          f"({values} values compared)"
          + (f"; worst {worst_overall[0]} at {worst_overall[1]:.2e} "
             f"({worst_overall[2]} ulp)" if worst_overall[0] else ""))
    if failures:
        print(f"{len(failures)} FAILED: "
              + " ".join(m for m, _, _ in failures))
        if args.update_ulp_ledger or first_reasons is not None:
            print("ULP ledger not updated: every model must pass first")
        return 1
    if args.update_ulp_ledger:
        write_ulp_ledger(ledger, args.target_platform, "main", measured, models,
                         {m: refs[m]["recorded"].get("platform") for m in models},
                         truth_measured=truth_measured)
        print(f"updated {ULP_LEDGER_PATH.relative_to(REPO)} for "
              f"{args.target_platform}, main")
    if first_reasons is not None:
        return report_first_entries(ledger, args.target_platform, "main",
                                    measured, first_reasons)
    return 0


if __name__ == "__main__":
    sys.exit(main())
