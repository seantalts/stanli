# brms performance and numerics

COM-Poisson now compiles and samples, including its parameter-dependent loop,
integer remainder, vector writes, and prefix reduction. Complete sampling
measures **0.838× CmdStan/Stanli**, compared with 0.573× for the matched previous
Stanli build. Stanli's median runtime fell 31.6%; it takes about 19% longer than
CmdStan on this workload.

All ratios below are **CmdStan / Stanli**: above one means Stanli is faster.
The COM-Poisson confirmation uses runtime `cb58eb22`; the later result-copy
comparison below uses `06fb1b90`. Both September 16, 2026 measurements supplement the
[199-model teaching report](../../output/teaching-performance/README.md).

## COM-Poisson sampling

Three confirmation rounds, each with seeds 1–4, give **0.840×, 0.843×, and
0.834×**. The headline ratio divides the medians of all 12 runtimes per engine.
All runs complete within the original cap. All **12 before/after Stanli draw
files are byte-identical**.

| Seed | Ratio of median runtimes | Range across three trials |
| --- | ---: | ---: |
| 1 | 0.988× | 0.972–1.000× |
| 2 | 0.810× | 0.801–0.820× |
| 3 | 0.815× | 0.801–0.828× |
| 4 | 0.799× | 0.798–0.807× |

The overall ratio exceeds the practical 0.8 target. Seed 4's unrounded median
is 0.79944, so this does not establish the target for every seed or trial.

The main cost was repeatedly initializing a 10,000-element workspace, even
on a branch that never used it. The compiler now delays that initialization,
removes overwritten constant stores, and reuses executor-owned buffers after
proving every read is initialized. Scalar callback buffers use Stan Math's
existing arena allocator. The model, math calls, derivatives, random seeds,
and sampler settings are unchanged by these optimizations.

## Numerical differences from CmdStan

ULP means one step between adjacent floating-point numbers. These are maximum
differences against independent CmdStan evaluations, computed before decimal
serialization.

| COM-Poisson quantity | Three corpus points: absolute | ULP | 28 posterior/boundary points: absolute | ULP |
| --- | ---: | ---: | ---: | ---: |
| Log density | 2.84e-14 | 2 | 1.14e-13 | 4 |
| Gradient | 2.13e-14 | 17 | 2.27e-13 | 127 |
| Constrained/generated outputs | 0 | 0 | 8.88e-16 | 1 |

Density, gradients, and outputs at the additional 28 points are bitwise
identical before and after the optimization. Earlier three-point checks for
asymmetric Laplace gave maximum absolute density/gradient differences of
1.42e-14/1.78e-14; the zero-inflated variant gave 4.26e-14/1.42e-14. Their
outputs matched exactly.

## Earlier GP and inverse-Gaussian results

Separate measurements cover the fixes integrated in `4db5dca2`. The measured
checkout was `6e462c2e` plus the candidate patch; its two changed runtime files
were verified to match `4db5dca2` exactly. These are historical results, separate
from both the original full sweep and the latest measurements above.

The three fixed-location exp-quad GP fixtures matched every recorded value at
all three reference points exactly on this machine. Including the Rethinking
m14.8 canary, the replay compared 900 values with a largest scaled discrepancy
of 3.00e-16. Existing cross-platform numerical policies remain unchanged.

All four seeds completed for each fixture, using 1,000 warmup iterations and
1,000 retained draws, unchanged inputs, and the original min(3 × CmdStan time,
900 seconds) cap. Ratios compare median complete CLI time, including Stanli
preparation and excluding CmdStan compilation.

| Fixture | CmdStan / Stanli | Divergences: Stanli / CmdStan |
| --- | ---: | ---: |
| `sw_gp` | 1.194× | 16 / 283 |
| `i320_gp_expquad` | 1.381× | 28 / 26 |
| `s2_gp_by_gr` | 1.492× | 2 / 4 |
| `s2_invgaussian` | 1.993× | 1689 / 1671 |

Inverse Gaussian samples in both engines, but the original fixed reference
points are outside its domain in both; the additional comparison below covers
valid points. All four fixtures retain
diagnostic flags, so these timings do not establish time to equally accurate
inference. The [corresponding PR CI](https://github.com/seantalts/stanli/actions/runs/35082758748)
passed its Linux runtime R and compiler checks.

### Additional inverse-Gaussian checks

Runtime `55bef134` agrees with CmdStan at 49 additional finite points: 16
preselected draws from the four existing CmdStan chains, 27 interior points,
and six near the positive linear-predictor boundary, down to a margin of 10⁻¹⁰.
The comparison includes sampling log density with the transform Jacobian,
all three unconstrained gradients, and all five output columns.

| Quantity | Maximum absolute difference | Maximum ULP |
| --- | ---: | ---: |
| Log density | 4.55e-13 | 2 |
| Gradient | 1.46e-11 | 116 |
| Constrained/transformed/generated outputs | 0 | 0 |

The three original reference points remain unchanged and produce nonfinite
density or gradients in both engines. The additional results supplement that
record; the earlier sampling diagnostic flags still apply.

## Removing redundant result copies

Runtime `06fb1b90` returns a region's single result directly, removing 40
identity copies from each Laplace model. It also prevents the loop optimizer
from treating distinct compiled programs as interchangeable merely because
their visible inputs match. A regression demonstrates the old mistake changing
a derivative from 21 to 6; the fix retains each program's own computation.

This separate comparison uses main `eb51be1f` as its matched baseline. Each
model has three rounds of seeds 1–4, with the same 1,000 warmup iterations,
1,000 retained draws, thread limits, executable preconditioning and caps as
above. All runs complete. Ratios divide the medians of all 12 CLI durations
per engine; they are not averages of the round ratios.

| Model | Before: CmdStan / Stanli | After: CmdStan / Stanli |
| --- | ---: | ---: |
| Asymmetric Laplace | 0.804× | 0.841× |
| Zero-inflated asymmetric Laplace | 0.834× | 0.853× |
| Mixture with estimated proportions | 0.788× | 0.786× |
| COM-Poisson | 0.843× | 0.849× |

Asymmetric Laplace's median Stanli runtime falls 4.4%; the zero-inflated
variant falls 2.3%. Asymmetric Laplace's three round ratios are **0.853×,
0.792× and 0.793×**, so the combined result does not establish 0.8× in every
round. The zero-inflated variant gives 0.901×, 0.825× and 0.847×; one round
has a slightly higher Stanli median than its matched baseline.

Mixture and COM-Poisson also include a second timing arm using the identical
baseline executable, with all six arm orders used twice. Median paired timing
differences between those identical arms are 1.6% and 0.7%, respectively.
The small canary changes above do not establish a performance improvement or
regression. Mixture remains below the practical target and has poor mixing
in both engines.

All **48 before/after Stanli draw files are byte-identical**, as are the 24
baseline/control pairs. Both Laplace models retain zero divergences and depth
hits, with maximum parameter R-hat below 1.01 in both engines. Mixture retains
64 Stanli divergences and 1,333 CmdStan divergences. These are fixed-budget
timings, not time to equally accurate inference.

Validation passes all 253 CTests, all 13 educational fixtures, the 316-model
reference replay under existing policies, and runtime R acceptance without
skips or warnings. Density, gradients and outputs for six targeted models at
three points each are bitwise identical to the baseline. No model, reference,
numerical threshold or Stan Math implementation changed.

## Remaining models

A preceding four-seed survey used `cb58eb22`. Each ratio divides
the two engines' median runtimes. This is one survey, not repeated confirmation
of a performance threshold.

| Model | CmdStan / Stanli | Completed seeds: Stanli | CmdStan |
| --- | ---: | ---: | ---: |
| Negative binomial with group effects (`sw_re_negbin`) | — | 3/4 | 4/4 |
| Mixture with estimated proportions (`s2_mixture_theta`) | 0.796× | 4/4 | 4/4 |
| Generalized extreme value (`s2_gev`) | — | 3/4 | 4/4 |
| Asymmetric Laplace (`sw_asymlaplace`) | 0.753× | 4/4 | 4/4 |
| Zero-inflated asymmetric Laplace (`s2_zi_asymlaplace`) | 0.817× | 4/4 | 4/4 |

Negative-binomial seed 2 and GEV seed 1 reached their caps; no aggregate ratio
is computed from surviving seeds. GEV's completed chains have zero divergences
and maximum-depth hits in both engines. Separate fixed-point profiling assigns
92.6% of gradient time to its loop executor; that is a gradient diagnostic,
not an end-to-end speed measurement.

Both Laplace fits have zero divergences and maximum-depth hits, with parameter
R-hats below 1.01 in both engines. The mixture has 64 divergences for Stanli
and 1,333 for CmdStan, with poor mixing in both. Fixed-budget runtime therefore
does not establish time to equally accurate inference.

### GEV close to zero shape

A separate 40-row stress fixture uses the [same GEV expression](../../tests/brms/s2_gev.stan)
with scale 1, responses `y[i] = (i - 20) / 8` and locations
`mu[i] = 0.025 * sin(i + 1)`, for `i = 0, ..., 39`. The table compares the
likelihood derivative with respect to shape against a 100-digit calculation
at the same double-precision inputs.

| Shape | Independent derivative | CmdStan / Stanli scalar-program derivative | Absolute error |
| --- | ---: | ---: | ---: |
| 0.25 | −1405.830841685 | −1405.830841685 | 2.78e-13 |
| 10⁻⁸ | −131.317854760 | −129.007145405 | 2.31 |
| 10⁻¹² | −131.317846817 | −26827812.078125 | 2.68e7 |

Across 27 evaluations, CmdStan and Stanli's scalar-program path give
bit-identical densities and gradients. The independent derivatives agree
with centered differences to a maximum scaled discrepancy of 1.51e-44.
These are synthetic stress points, not posterior draws; they do not establish
that the earlier sampled GEV chains visit this regime or explain their timing.

### Negative-binomial numerical limitation

The fast CmdStan seed has 982 divergences in 1,000 retained draws and averages
19.78 leapfrog steps per draw, versus 809–972 in its other seeds. Both engines
visit extremely large dispersion values, with enormous positive log densities
and tiny adapted step sizes.

A standalone program using only the pinned upstream Stan Math library
reproduces precision loss in `neg_binomial_2_log_glm_lpmf`. For 40 observations
equal to 3, each with mean 3:

| Dispersion | GLM log probability | Equivalent non-GLM log probability | Absolute difference |
| --- | ---: | ---: | ---: |
| 10⁸ | −59.83695984 | −59.83690473 | 5.51e-5 |
| 10¹² | −60.25 | −59.83690413 | 0.4131 |
| 10¹⁶ | 6144 | −59.83690413 | 6203.84 |
| 10²² | 17179869184 | −59.83690413 | 1.72e10 |

The GLM subtracts very large terms that should nearly cancel. The equivalent
`neg_binomial_2_log_lpmf` call agrees within 2.1e-12 with an independent
finite-product calculation over seven dispersion values from 2 to 10¹⁰⁰.
This demonstrates a library limitation in a regime visited by the chains;
it does not identify the cause of every divergent proposal. The model and
upstream density implementation remain unchanged.

Reverse-mode gradients also lose precision. For the same 40 observations,
the table below gives the derivative of log probability with respect to
**log dispersion**, the scale used by the sampler. The independent calculation
uses the finite-product identity at 360 decimal digits; its derivatives agree
with centered differences to relative error below 1.9e-65 across 63 input points.
The C++ evaluations call only the pinned upstream Stan Math library.

| Dispersion | Independent derivative | GLM derivative | Non-GLM derivative |
| --- | ---: | ---: | ---: |
| 10⁴ | 0.00599840045 | 0.00599840149 | 0.00599840114 |
| 10⁸ | 5.99999984e-7 | 0 | −2.48201017e-6 |
| 10¹² | 6.00e-11 | 0 | −0.0603862 |
| 10¹⁶ | 6.00e-15 | 0 | −120 |

The non-GLM function's accurate log probability therefore does not establish
an accurate gradient. A further case with 40 observations equal to 12, mean 10
and dispersion 10¹² gives a GLM log-dispersion derivative of −0.113687 versus
an independent value of 1.60e-10. These are likelihood derivatives; priors and
the parameter-transform Jacobian are excluded. No replacement or correction
to either upstream implementation is applied.

## Method and validation

- Apple M3 Ultra, macOS ARM64, Release build; Stanli `cb58eb22`, matched previous
  build `670e0039`, CmdStan `11cb052d`, and Stan Math `8f326d14`.
- Each seed uses 1,000 warmup iterations and 1,000 retained draws. Executables
  receive one no-argument launch before timing. These are warm-executable CLI
  measurements, including Stanli source compilation and CSV output, excluding
  CmdStan model compilation and installation.
- CmdStan runs first for each seed; the cap is min(3 × its duration, 900 seconds).
  Previous/candidate Stanli order alternates in the COM-Poisson confirmation.
  Runs are serial with numerical thread limits set to one. Builds, tests,
  profiling, and diagnostics run separately from timing.
- Validation passed 253 CTests, 316 corpus reference checks covering 1,008,755
  values under the existing policies, and 495 R expectations without failures,
  warnings, or skips. A separate instrumented replay check also passed all 316
  references; that instrumentation is absent from the measured executable.
- No Stan Math source changes were introduced. The dependency setup's older
  adjoint ODE initialization patch in [deps/fetch.sh](../../deps/fetch.sh) predates
  this work and is unrelated to these models.

The [benchmark protocol](../../docs/benchmarks.md#how-we-measure) documents the runner, numerical
gate, thread settings and retained failures. Current runs measure setup and warm gradients and
report an explicit setup-plus-20,000-gradient time estimate; the sampling evidence
above belongs to this historical experiment. For a fresh run, use
`harnesses/corpus_bench.py` with `--corpus brms --filter s2_com_poisson`. [TESTING.md](../../TESTING.md) covers build setup and validation:

```sh
ctest --test-dir build-rel --output-on-failure
python3 tools/verify_refs.py deps/posteriordb \
  --check build-rel/stanli_check --jobs 8 --per-model
```

Raw trials, failed experiments, profiles, and review logs are retained outside
the source tree. The original full-sweep and Rethinking results remain unchanged.
