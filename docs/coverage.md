# Probability-function coverage

stanli implements most Stan probability functions, the common scalar math
functions, and the parameter transforms listed below, but not every Stan Math
overload. If you use an unsupported form, stanli stops with an error. It never
substitutes an approximation.

This page covers probability functions, real-valued scalar math, parameter
transforms, truncation, censoring, `print` and `reject`. For whole models, see
[the table of every model](../TESTING.md#every-model). For language constructs, see the
[stanc3 test models](../tests/stanc3_good/README.md). For how the checks work
and their numerical limits, see [TESTING.md](../TESTING.md).

## Coverage at a glance

The checked-in conformance baseline
([`conformance-baseline.json.gz`](internal/artifacts/conformance-baseline.json.gz))
was produced with stanc3 2.40.0 (`d58446e6`). It holds 24,291 Stan Math
signatures and 31 language-construct cases, of which 19,035 are verified
against CmdStan. A separate generated replay covers 7,407 builtin and 12,131
density signatures in 66,698 cases across 73 models, including
cases that mix data and parameter arguments.

| family | name metric | verified | unexpected unsupported | generator gaps | inapplicable |
| --- | ---: | ---: | ---: | ---: | ---: |
| densities (`_lpdf`, `_lpmf`) | 73 / 73 | 3,858 | 0 | 188 | 9 |
| distribution functions (`_cdf`, `_lcdf`, `_lccdf`) | 108 / 108 | 8,068 | 0 | 18 | 24 |
| scalar math | 100 / 100 | 95 | 0 | 0 | 5 |

The name metric counts names with no `unexpected_unsupported` signature. It is
a regression count. A name can pass it without every overload working.

The status columns count signatures. For densities and distribution functions
they cover every signature in the inventory. The scalar-math row counts names
that have a signature with one to five `real` arguments and a `real` result. It
excludes densities, distribution functions, RNGs and quantiles (`_qf`,
`_log_qf`). Its status columns cover only those 100 all-real signatures.
The baseline also verifies all 256 `student_t_qf` overloads.

## How to read the classifications

Each signature receives one status:

| status | meaning |
| --- | --- |
| `verified` | CmdStan and stanli evaluated the generated case and passed its numerical check. |
| `unexpected_unsupported` | CmdStan accepted the case and stanli rejected it. This is implementation backlog. |
| `generator_gap` | The harness cannot yet generate a valid differential case. It says nothing about support. |
| `inapplicable` | No gradient comparison applies, for example an RNG-only case, a case with no real input or result, or a zero Stan gradient. |
| `expected_unsupported` | Left unsupported after review. Currently this means complex values and tuple-valued results. |
| `mismatch` | Both answered, and the results failed the numerical or semantic check. |
| `crashed` | The stanli worker died before returning a result. |
| `harness_error` | The harness could not construct or compare the case. |

## Numerical agreement

For each applicable generated signature, the nightly conformance run builds
equivalent CmdStan and stanli models, evaluates three deterministic parameter
points, and compares the log density, every gradient component, and whether
either side rejects the point. The limit is 10 ULP, with reviewed exceptions
where ULP is the wrong scale. `wiener_lpdf` uses a `1e-12` relative limit on the
log density and every gradient component, because some near-zero gradient
lanes vary with container instantiation and compiler optimization. The policy
is in [TESTING.md](../TESTING.md#numerical-policy).

Two differences in the numbers are known and expected. The first is between
platforms. On arm64, Eigen's vectorized kernels use fused multiply-add
instructions, and the default x86-64 build does not use them, so the two
platforms can differ by a few ULP in matrix products, reductions and vectorized
`exp` and `log`. The cause is in Eigen, which CmdStan also uses, so a CmdStan
reference that one platform recorded can differ from stanli on the other.
[TESTING.md](../TESTING.md#numerical-policy) describes how the matrix product
case is handled. The second is between stanli and Stan Math. In the graph kernel
for `logit`, stanli divides the incoming adjoint by `x * (1 - x)` where Stan
Math multiplies it by `1 / (x - x * x)`. Near `x = 1` the second form loses
digits to cancellation. Against exact arithmetic stanli is within one ULP at
`x = 0.999`, and Stan Math is about 250 ULP away.

Under `~`, stanli drops the same data-only terms as CmdStan; see
[lp__ and dropped constants](lp-constant.md).

## Known unsupported forms

The name metric in the table above does not show these gaps:

- Extended `wiener_lpdf`. The five-argument form works. The six- to
  nine-argument forms are refused because the kernel layout has five inputs
  (six unexpected-unsupported signatures and two generator gaps).
- `wiener_lcdf_unnorm` and `wiener_lccdf_unnorm` with all-real arguments have
  no graph operation and are refused.
- `gp_dot_prod_cov` is refused. The other five GP kernels work in every
  form that Stan has, square and cross-covariance: exponentiated quadratic,
  Matern 3/2, Matern 5/2, exponential and periodic.
- Complex arguments and results are refused by policy. The graph holds real
  and integer values only.
- Tuples work in data, parameters, locals, generated quantities and user
  functions; stanli replaces each with one variable per component before
  running the model. Three library functions return tuples stanli computes:
  `qr`, `qr_thin` and `eigendecompose_sym` of a real matrix. The others
  (`svd`, `csr_extract`, the complex decompositions and
  `generate_laplace_options`) are refused, as are an array of tuples
  declared as a parameter and `print` or `reject` of an array of tuples.
  The conformance sweep still classifies tuple-valued results as expected
  unsupported, since its generator does not build them.

stanli records whether each GLM argument was a scalar or an array. Scalar
outcomes and trial counts then use Stan Math's broadcast overload. This
affects the normalizing constant of functions such as `poisson_log_glm_lpmf`.
Per-row vector intercepts work alongside scalar ones.

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
representative declarations. It compares the unconstrained size, the total log
density including the Jacobian, the full gradient, and rejection with CmdStan
at three points, within 2 ULP. CI separately checks representative sizes,
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
density within a small rounding tolerance. In a comparison by hand, CmdStan
differed by 1 ULP in the log density on the scalar fixture and not at all on
the vector one.
Truncating a parameter variate needs a parameter-dependent support check that
stanli does not support yet.

In Stan you write censoring explicitly. It works wherever stanli supports the
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
Stan Math template directly. That operation records partials into scratch space
or runs a nested Stan Math tape, which costs some allocation and tape work.
Adding a function is one X-macro entry only when an existing layout fits. See
[the contributor guide](hacking.md) for details.

## Reproducing

`python3 tools/coverage_summary.py` prints the name-metric rows above:

```text
densities (73, 73)
distribution functions (108, 108)
scalar math (100, 100)
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

A sliced run reports `partial_run`, which is not a failure. The
[conformance harness README](../harnesses/conformance/README.md) covers setup,
artifacts, pass and fail rules, and snapshot updates.
