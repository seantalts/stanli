# Probability-function coverage

stanli implements most Stan probability functions, the common scalar math
functions, and the parameter transforms listed below, but not every Stan Math
overload. It refuses an unsupported form with an error and never substitutes
an approximation.

This page covers probability functions, real-valued scalar math, parameter
transforms, truncation, censoring, `print` and `reject`. For whole models see
[corpus status](corpus-status.md); for language constructs see the
[stanc3 fixtures](../tests/stanc3/README.md); for how the checks work and
their numerical limits see [TESTING.md](../TESTING.md).

## Coverage at a glance

The checked-in conformance baseline
([`conformance-baseline.json.gz`](internal/artifacts/conformance-baseline.json.gz))
was produced with stanc3 2.40.0 (`d58446e6`). It holds 24,291 Stan Math
signatures and 31 language-construct cases, of which 19,009 are verified
against CmdStan. A separate generated replay covers 7,387 builtin and 12,131
density signatures as 66,678 cases across 73 models, including mixed
data/parameter arguments.

| family | name metric | verified | unexpected unsupported | generator gaps | inapplicable |
| --- | ---: | ---: | ---: | ---: | ---: |
| densities (`_lpdf`, `_lpmf`) | 72 / 73 | 3,852 | 6 | 188 | 9 |
| distribution functions (`_cdf`, `_lcdf`, `_lccdf`) | 108 / 108 | 8,068 | 0 | 18 | 24 |
| scalar math | 94 / 100 | 89 | 6 | 0 | 5 |

The name metric counts names with no `unexpected_unsupported` signature. It is
a regression count, and a name can pass it without every overload working. For
example, the classic five-argument `wiener_lpdf` works and its extended
overloads do not. `gaussian_dlm_obs_lpdf` has no verified overload, yet passes
the metric because its cases are generator gaps.

The status columns count signatures. For densities and distribution functions
they cover every signature in the inventory. The scalar-math row counts names
that have a signature with one to five `real` arguments and a `real` result,
excluding densities, distribution functions, RNGs and quantiles (`_qf`,
`_log_qf`); its status columns cover only those 100 all-real signatures.
`hypergeometric_1F0(real, real)` has gained support since the baseline was
recorded and is covered by fresh CmdStan references, so five scalar names
remain missing. The baseline also verifies all 256 `student_t_qf` overloads.

## How to read the classifications

Each signature receives one status:

| status | meaning |
| --- | --- |
| `verified` | CmdStan and stanli evaluated the generated case and passed its numerical gate. |
| `unexpected_unsupported` | CmdStan accepted the case and stanli rejected it. This is implementation backlog. |
| `generator_gap` | The harness cannot yet generate a valid differential case. This is no evidence of support. |
| `inapplicable` | No gradient comparison applies, for example an RNG-only case, a case with no real input or result, or a zero Stan gradient. |
| `expected_unsupported` | A reviewed boundary, currently complex values and tuple-valued results. |
| `mismatch` | Both answered, and the results failed the numerical or semantic gate. |
| `crashed` | The stanli worker died before returning a result. |
| `harness_error` | The harness could not construct or compare the case. |

## Numerical agreement

For each applicable generated signature, the nightly conformance run builds
equivalent CmdStan and stanli models, evaluates three deterministic parameter
points, and compares the log density, every gradient component, and whether
either side rejects the point. The limit is 10 ULP, with reviewed exceptions
where ULP is the wrong scale. `wiener_lpdf` uses a `1e-12` relative gate on the
log density and every gradient component, because some near-zero gradient
lanes vary with container instantiation and compiler optimization. The policy
is in [TESTING.md](../TESTING.md#numerical-policy).

Some less common densities match CmdStan's gradients and report `lp__` shifted
by a constant; see [lp off by a constant](lp-constant.md).

## Known unsupported forms

The headline ratios hide these gaps:

- Extended `wiener_lpdf`. The five-argument form works. The six- to
  nine-argument forms are refused because the kernel layout has five inputs
  (six unexpected-unsupported signatures and two generator gaps).
- `gaussian_dlm_obs_lpdf`. Every call is refused: it takes seven arguments and
  a graph operation has at most six input slots. Its two rows are generator
  gaps, so the 72/73 metric does not show it.
- `discrete_range_cdf`, `discrete_range_lcdf` and `discrete_range_lccdf`.
  These are not lowered onto the log-density graph, because all their inputs
  are integers. The harness marks all 24 signatures inapplicable.
- `hypergeometric_2F1`, `inc_beta`, `inv_inc_beta`, `wiener_lcdf_unnorm` and
  `wiener_lccdf_unnorm` with all-real arguments have no graph operation and are
  refused.
- `gp_periodic_cov` has no graph operation and is refused. The
  exponentiated-quadratic, Matern 3/2, Matern 5/2 and exponential kernels work.
- Complex arguments and results are refused by policy. The graph holds real
  and integer values only.
- Tuple-valued results are refused by policy. The graph cannot yet represent
  or destructure tuples.

GLM calls keep whether each argument was a scalar or an array, so scalar
outcomes and trial counts use Stan Math's broadcast overload, which matters to
the normalizing constant of functions such as `poisson_log_glm_lpmf`. Per-row
vector intercepts work alongside scalar ones.

## Parameter transforms

`n` is the number of scalar elements before any structure-specific reduction.

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

Arrays of structured parameters work; multiply the per-value size by the
number of elements. Offset and multiplier expressions may be data or
parameters, scalar or per element.

[`harnesses/transform_sweep.py`](../harnesses/transform_sweep.py) covers 30
representative declarations. At three points it compares the unconstrained
size, the total log density including the Jacobian, the full gradient, and
rejection with CmdStan, at 2 ULP. CI separately checks representative sizes,
finite-difference gradients and constraint properties. This does not cover
every legal array shape or dimension expression.

## Truncation and censoring

Stan's `T[lower, upper]` means truncation. stanc rewrites a two-sided truncated
sampling statement as a proportional density, a normalization term, and
support checks on the variate:

```stan
target += normal_lupdf(y | mu, sigma);
target -= log_diff_exp(normal_lcdf(upper | mu, sigma),
                       normal_lcdf(lower | mu, sigma));
```

stanli supports the scalar and vectorized forms in
`tests/fixtures/trunc.stan` and `tests/fixtures/truncvec.stan`. CI compares
them with a Stan Math reverse-mode reference: gradients exactly, the log
density within a small rounding tolerance. By hand, CmdStan differed by 1 ULP
in the log density on the scalar fixture and not at all on the vector one.
Truncating a parameter variate needs a parameter-dependent support check that
stanli does not support yet.

Censoring is written explicitly in Stan, and works wherever stanli supports the
CDFs and the surrounding control flow:

```stan
target += normal_lcdf(limit | mu, sigma);   // left-censored
target += normal_lccdf(limit | mu, sigma);  // right-censored
target += log_diff_exp(normal_lcdf(upper | mu, sigma),
                       normal_lcdf(lower | mu, sigma));  // interval-censored
```

## `print`, `reject`, and parameter-dependent control

`print` and `reject` work in transformed data and in ordinary model
statements:

- A taken `reject` in transformed data stops the model from being constructed.
- In the model block, `reject` throws `std::domain_error`, which the sampler
  treats as a rejected proposal.
- `print` sends one line to a configurable message sink, standard output by
  default. It supports at most six interpolated values. Scalars and flat
  vectors format like CmdStan; nested containers and matrices do not yet.

stanli refuses `print` or `reject` inside a parameter-dependent region that it
replays, because the reverse pass could run it twice.

Parameter-dependent `if` and ternary expressions work. `for` loops with
bounds known at load time are unrolled, and `break` and `continue` inside
parameter-dependent branches become runtime jumps. A `while` loop works only
when its condition can be evaluated on every iteration while the region is
built. Probability functions (sampling statements, vectorized calls, discrete
distributions, CDFs, GLMs and matrix densities) behave the same inside these
regions as outside, because one registry describes their arguments for every
execution path. That includes the vector-or-array forms of `multi_normal`,
`multi_normal_prec`, `multi_normal_cholesky`, `multi_student_t`,
`multi_student_t_cholesky`, `dirichlet`, and per-observation cutpoints for
the ordered logistic and probit densities. Generated fixtures instantiate all
12,131 density signatures of the 177 registered probability functions
(excluding the eight `wiener_lpdf` signatures) in transformed data, the model
block, parameter-dependent control and generated quantities. Broad conformance
cases for parameter-dependent control remain `unexpected_unsupported`;
focused fixtures cover the implemented forms.

## How functions enter the runtime

A Stan function reaches the runtime in one of three ways. It can lower to
existing graph operations when that preserves the value, the validation checks
and Stan's `propto` behavior. It can reuse an existing kernel layout when its
shapes and activity rules match one. Otherwise a runtime operation calls the
Stan Math template directly, recording partials into scratch space or running
a nested Stan Math tape, at some allocation and tape cost. Adding a function is
one X-macro entry only when an existing layout fits. See
[the contributor guide](hacking.md) for details.

## Reproducing

`python3 tools/coverage_summary.py` prints the name-metric rows above:

```text
densities (72, 73)
distribution functions (108, 108)
scalar math (94, 100)
```

The full differential run is
[`harnesses/stan_conformance.py`](../harnesses/stan_conformance.py). After
`tools/dev_setup.sh --conformance`, run a slice with `--filter`, which selects
every signature containing the text:

```sh
.venv-conformance/bin/python harnesses/stan_conformance.py \
  --stanc deps/stanc3/stanc-pinned --cmdstan deps/cmdstan \
  --build build-rel --stanli-pythonpath python --filter normal_lpdf
```

A sliced run reports `partial_run`, which describes its scope and is no
failure. The [conformance harness README](../harnesses/conformance/README.md)
covers setup, artifacts, gates and snapshot updates.
