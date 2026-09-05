# Probability-function coverage

## Abstract

stanli implements most Stan probability functions, common scalar math
functions, and the parameter transforms listed below, but not every Stan Math
overload.

In the pinned stanc inventory, 71 of 72 density names, all 105
`_cdf`/`_lcdf`/`_lccdf` names, and 94 of 100 scalar-math names have no
`unexpected_unsupported` signature. These are name-level regression counts,
not counts of fully implemented functions. The classic five-argument
`wiener_lpdf` is implemented, for example, but its extended overloads are not.
`gaussian_dlm_obs_lpdf` has no verified overload, yet does not reduce the 71/72
count because its cases remain generator gaps.

stanli refuses unsupported forms rather than substituting an approximation.
For applicable generated cases, the conformance harness compares the log
density and full unconstrained gradient with CmdStan at three deterministic
parameter points. The usual limit is 10 units in the last place (ULP).
Documented exceptions use a function-specific absolute or relative tolerance.

This page covers probability functions, real-valued scalar math, parameter
transforms, truncation, censored likelihood terms, `print`, and `reject`. It is
not a complete language-coverage table. See [model coverage](corpus-status.md),
[language-construct coverage](../tests/stanc3/README.md), and
[the test-oracle description](../TESTING.md) for those scopes.

## Coverage at a glance

The checked-in classification baseline was produced with
`stanc3 v2.39.0-76-gac69570 (Unix)`. The current embedded-compiler pin is a
newer revision (`5b824ee`), but its signature dump has the same SHA-256
(`7ae665c2d1ea5f49084cb3d4b5eff41791d4a465cedd5728519c9782d2fe77a1`)
and contains the same 24,246 Stan Math signatures. The baseline also contains
31 language-construct cases. A new full run is still needed to refresh the
compiler metadata recorded in the baseline.

| family | name metric | verified | unexpected unsupported | generator gaps | inapplicable |
| --- | ---: | ---: | ---: | ---: | ---: |
| densities (`_lpdf`, `_lpmf`) | 71 / 72 | 3,852 | 6 | 170 | 9 |
| distribution functions (`_cdf`, `_lcdf`, `_lccdf`) | 105 / 105 | 8,068 | 0 | 0 | 24 |
| scalar math | 94 / 100 | 89 | 6 | 0 | 5 |

The name metric counts names with no `unexpected_unsupported` signature; the
four status columns count signatures. For densities and distribution
functions, those columns cover every inventory signature. For scalar math,
they cover only the 100 qualifying all-real signatures. The scalar name metric
still checks every inventory signature attached to each qualifying name.

The scalar-math row counts a name if it has a signature with one to five
`real` arguments and a `real` result. It excludes densities, distribution
functions, RNGs, and quantiles (`_qf` and `_log_qf`). The six missing names
are listed under [known unsupported forms](#known-unsupported-forms).
`student_t_qf` is also not implemented, but quantiles are outside this metric.

### How to read the classifications

A name may have many signatures for scalar, vector, array, and matrix argument
shapes. Each signature receives one status:

| status | meaning |
| --- | --- |
| `verified` | CmdStan and stanli evaluated the generated case and passed its numerical gate. |
| `unexpected_unsupported` | CmdStan accepted the case and stanli rejected it; this is an implementation backlog item. |
| `generator_gap` | The harness cannot yet generate a valid differential case; this is not evidence of support. |
| `inapplicable` | No gradient comparison applies, for example because a case is RNG-only, has no real input or result, or has a zero Stan gradient. |
| `expected_unsupported` | A reviewed boundary, currently complex values and tuple-valued results. |
| `mismatch` | Both implementations answered, but their results failed the numerical or semantic gate. |
| `crashed` | The stanli worker died before returning a result. |
| `harness_error` | The harness could not construct or compare the case. |

A name fails the name-level metric only when at least one signature is
`unexpected_unsupported`. Generator gaps and inapplicable signatures do not
reduce the count. The metric detects regressions against the baseline, but the
unsupported list below is needed to interpret it.

## Numerical agreement

The nightly conformance run provides the broadest numerical check. For each
applicable generated signature, it builds equivalent CmdStan and stanli models,
evaluates three deterministic parameter points, and compares:

- the log density;
- every component of the unconstrained gradient;
- whether either implementation rejects the point.

The default gate is 10 ULP. Function-specific policies apply when ULP is the
wrong scale. For `wiener_lpdf`, a name-wide `1e-12` relative gate applies to
the log density and every gradient component. It was introduced for near-zero
gradient lanes that vary with container instantiation and compiler
optimization. The focused `fn_sweep.py` developer check uses a 2 ULP limit.
Some unit tests require exact equality when both sides intentionally use the
same Stan Math path, but exact equality is not the general coverage criterion.

Most common densities use the same activity mask as generated Stan, so both
retain or remove the same log-density constants. Some less common densities
use a compact kernel that treats every argument as active. For these kernels,
gradient agreement is the primary contract, and `lp__` may differ by a
parameter-independent offset. The offset may have either sign and matters when
an absolute log density is required. It does not affect Hamiltonian dynamics
or Metropolis acceptance ratios within one model. See
[compact densities](compact-densities.md).

## Known unsupported forms

The headline ratios hide several important gaps:

- **Extended `wiener_lpdf`:** the classic five-argument form is implemented;
  six- through nine-argument forms are refused. The kernel and lowering layout
  have five inputs. The pinned inventory contains six unexpected-unsupported
  extended signatures and two generator gaps.
- **`gaussian_dlm_obs_lpdf`:** all calls are refused. The function takes seven
  arguments, but each graph operation stores at most six input slots. Its two
  inventory rows are generator gaps, so the 71/72 metric does not show this
  gap.
- **`discrete_range_cdf`, `discrete_range_lcdf`, and
  `discrete_range_lccdf`:** these are not lowered onto the log-density graph.
  All inputs are integers, so the probability-kernel layout has no
  differentiable real edge. The harness marks all 24 signatures inapplicable,
  not unsupported.
- **Missing scalar math:** all-real scalar calls to `hypergeometric_1F0`,
  `hypergeometric_2F1`, `inc_beta`, `inv_inc_beta`,
  `wiener_lcdf_unnorm`, and `wiener_lccdf_unnorm` are refused because no graph
  operation is registered.
- **`student_t_qf`:** this quantile is refused and lies outside the scalar-math
  count. It has no graph operation.
- **`gp_periodic_cov`:** this covariance is refused. It has no graph
  operation. The exponentiated-quadratic, Matern 3/2, Matern 5/2 and
  exponential kernels are supported.
- **Complex values:** complex arguments or results are refused by policy. The
  graph represents real and integer values, not complex values.
- **Tuple results:** tuple-valued results are refused by policy. The graph
  cannot yet represent or destructure tuples.

### GLM argument shapes

GLM integer groups retain whether each source argument was a language scalar
or an array. Scalar outcomes and trial counts therefore use Stan Math's native
broadcast overload rather than being copied into an array, which matters to
the normalized constant in functions such as `poisson_log_glm_lpmf`.
Per-row vector intercepts are supported alongside scalar intercepts. This
shape payload is produced identically by graph lowering, runtime-control
compilation, and MIR interpretation; kernels retain compatibility with the
older `{rows, columns}` payload used by direct and serialized graphs.

## Parameter transforms

The runtime supports the parameter declarations below. Here, `n` is the number
of scalar elements before any structure-specific reduction.

| declaration or transform | unconstrained size per value |
| --- | ---: |
| unconstrained scalar, vector, row vector, or matrix | `n` |
| `lower`, `upper`, or `lower, upper` | `n` |
| `offset`, `multiplier`, or both | `n` |
| `simplex[K]` | `K - 1` |
| `sum_to_zero_vector[K]` | `K - 1` |
| `sum_to_zero_matrix[M, N]` | `(M - 1)(N - 1)` |
| `unit_vector[K]` | `K` |
| `ordered[K]`, `positive_ordered[K]` | `K` |
| `corr_matrix[K]` | `K(K - 1) / 2` |
| `cov_matrix[K]` | `K(K + 1) / 2` |
| `cholesky_factor_corr[K]` | `K(K - 1) / 2` |
| `cholesky_factor_cov[M, N]` | `N(N + 1) / 2 + (M - N)N` |

Arrays of structured parameters are supported. Multiply the per-value size by
the number of array elements. Compatible offset and multiplier expressions may
be data or parameters, and may be scalar or per-element containers.

`harnesses/transform_sweep.py` covers 30 representative declarations. Its
generated target reads every constrained component. At three attempted points,
the sweep compares the unconstrained size, total log density (including the
target expression and declaration Jacobian), full gradient, and mutual
rejection with CmdStan. Numeric comparisons use a 2 ULP gate. CI separately
checks representative sizes, central finite-difference gradients, and selected
constraint properties. This is not an inventory of every legal array shape or
dimension expression.

## Truncation and censored likelihoods

Stan's `T[lower, upper]` syntax means truncation, not censoring. Stanc rewrites
a two-sided truncated sampling statement as a proportional density, a
normalization term, and support checks on the variate. For a continuous
distribution, the main target contribution is:

```stan
target += normal_lupdf(y | mu, sigma);
target -= log_diff_exp(normal_lcdf(upper | mu, sigma),
                       normal_lcdf(lower | mu, sigma));
```

stanli supports the scalar and vectorized normal-truncation forms in
`tests/fixtures/trunc.stan` and `tests/fixtures/truncvec.stan`. CI compares
them with an in-process Stan Math reverse-mode reference. The focused tests
require exact gradients and allow a small log-density rounding tolerance. A
manual CmdStan comparison found a 1 ULP log-density difference for the scalar
fixture and no difference for the vector fixture.

Truncation coverage is not complete. In the nightly case where the truncated
variate is a parameter, stanli reaches an unsupported parameter-dependent
support check. The case is `unexpected_unsupported`.

Censoring is written explicitly in Stan. These are the usual contributions:

```stan
target += normal_lcdf(limit | mu, sigma);   // left-censored
target += normal_lccdf(limit | mu, sigma);  // right-censored
target += log_diff_exp(normal_lcdf(upper | mu, sigma),
                       normal_lcdf(lower | mu, sigma));  // interval-censored
```

These terms work when stanli supports the CDF functions and surrounding
control flow. The runtime has no separate censoring operator.

## `print`, `reject`, and parameter-dependent control

`print` and `reject` work in transformed data and ordinary graph-lowered model
statements, within the current message-layout limits.

- Transformed-data statements run during model preparation. A taken `reject`
  prevents the model from being constructed.
- Model-block statements run during evaluation. `reject` throws
  `std::domain_error`, which the sampler treats as a rejected proposal.
- `print` sends one rendered line to a configurable message sink. The default
  sink writes to standard output.

Graph-lowered messages support at most six interpolated values. Focused tests
cover scalar and flat-vector formatting; nested-container and matrix formatting
is not yet equivalent to CmdStan.

Runtime regions implement parameter-dependent `if` and ternary expressions.
`for` loops with load-time bounds are unrolled; `break` and `continue` inside
parameter-dependent branches become runtime jumps. A `while` loop works only
when its condition can be evaluated during region construction on every
iteration. Probability functions backed by graph kernels, including sampling
syntax (`~`), vectorized calls, discrete distributions, CDFs, GLMs, and matrix
density functions, use the same kernel ABI in parameter-dependent regions.
Their argument shapes, integer payloads, propto flag, and activity mask come
from one shared registry also used by ordinary graph lowering and MIR
interpretation. This includes `categorical_lpmf` and
`categorical_logit_lpmf`; their atomic vector argument and scalar-versus-array
outcome selection are a registry shape policy rather than backend-specific
dispatch. `hypergeometric_lpmf` and `discrete_range_lpmf` use the registry's
all-integer evaluation policy: because they have no differentiable edge, each
backend reaches the same scalar-or-array Stan Math evaluator (through one
generic runtime kernel where evaluation must be deferred), retaining
validation and `propto` semantics.
Multivariate vectorization is likewise a registry policy rather than a
backend exception. `multi_normal_lpdf`, `multi_normal_prec_lpdf`,
`multi_normal_cholesky_lpdf`, `multi_student_t_lpdf`, and
`multi_student_t_cholesky_lpdf` all accept a vector or array of vectors for
both the random variable and location. The registry centrally validates the
shared widths, distinguishes a single vector from an array of one vector, and
requires equal array lengths when both arguments are arrays; graph lowering,
runtime-control programs, and MIR interpretation consume the same encoded
layout.
The same vector-layout policy covers `dirichlet_lpdf` when either argument is
an array of vectors, and the ordered-logistic/probit densities when cutpoints
are supplied as one vector per observation.
An automatically generated source fixture instantiates all 3,003 density
signatures reported by the pinned `stanc --dump-stan-math-signatures` (70
functions after excluding Wiener and Gaussian-DLM), executes them in
transformed data, ordinary autodiff, parameter-dependent runtime control, and
generated quantities, and is checked for dump drift by CTest.
The explicitly unsupported probability functions listed above remain outside
that registry.

stanli also refuses a `print` or `reject` inside a replayed
parameter-dependent region because reverse-mode replay could execute it twice.
Broad conformance cases for parameter-dependent control and effects remain
`unexpected_unsupported`; focused fixtures cover the implemented forms.

## Appendix: how functions enter the runtime

There are three main implementation paths:

1. **Exact decomposition.** Lower the call to existing graph operations only
   when the decomposition preserves the returned value, validation checks, and
   Stan's `propto` behavior.
2. **Existing kernel layout.** Reuse an argument and derivative layout when the
   new function has the same shapes and activity rules as an implemented one.
3. **Precompiled Stan Math kernel.** Call the original Stan Math template from
   a runtime operation. Common functions can record their partial derivatives
   into operation scratch space. Functions that do arithmetic on autodiff
   scalars may use a nested Stan Math tape inside the operation.

The third path preserves coverage but retains some dynamic allocation and tape
cost. Adding a function is one X-macro entry only when an existing layout fits;
novel argument layouts, matrix-valued arguments, unusual validation, or nested
autodiff may require explicit lowering and kernel code. See
[the contributor guide](hacking.md) for the source-level details.

## Reproduce the coverage summary

The checked-in baseline records each pinned signature's classification, but
not the numerical deviations from the run that produced it. This script
reproduces the three name-level rows above:

```sh
python3 - <<'PY'
import collections
import gzip
import json
import re

with gzip.open("docs/conformance-baseline.json.gz", "rt") as handle:
    rows = json.load(handle)["classifications"]

by_name = collections.defaultdict(list)
for signature, result in rows.items():
    name = signature.split("(", 1)[0]
    by_name[name].append((signature, result["status"]))

def tally(names):
    names = sorted(names)
    covered = sum(
        all(status != "unexpected_unsupported" for _, status in by_name[name])
        for name in names
    )
    return covered, len(names)

densities = {
    name for name in by_name if name.endswith(("_lpdf", "_lpmf"))
}
cdfs = {
    name for name in by_name if name.endswith(("_cdf", "_lcdf", "_lccdf"))
}

excluded = (
    "_lpdf", "_lpmf", "_rng", "_cdf", "_lcdf", "_lccdf", "_qf", "_log_qf"
)
scalar_math = set()
pattern = re.compile(r"^([A-Za-z_][A-Za-z_0-9]*)\((.*?)\)=>(.*)$")
for signature in rows:
    match = pattern.match(signature)
    if not match:
        continue
    name, arguments, result = match.groups()
    argument_types = arguments.split(",") if arguments else []
    if (
        result == "real"
        and 1 <= len(argument_types) <= 5
        and all(argument == "real" for argument in argument_types)
        and not name.endswith(excluded)
    ):
        scalar_math.add(name)

print("densities", tally(densities))
print("distribution functions", tally(cdfs))
print("scalar math", tally(scalar_math))
PY
```

Expected output for the current baseline:

```text
densities (71, 72)
distribution functions (105, 105)
scalar math (94, 100)
```

The full differential run is `harnesses/stan_conformance.py`; setup, slicing,
artifact layout, gates, and snapshot updates are documented in
[the conformance harness README](../harnesses/conformance/README.md). After
`tools/dev_setup.sh --conformance`, run a smaller slice with the same harness:

```sh
.venv-conformance/bin/python harnesses/stan_conformance.py \
  --stanc deps/stanc3/stanc-pinned \
  --cmdstan deps/cmdstan \
  --build build-rel \
  --stanli-pythonpath python \
  --filter normal_lpdf
```

`--filter` selects every canonical signature containing the supplied text,
case-insensitively. A sliced run reports `partial_run`; that warning describes
its requested scope rather than a conformance failure.
