# Teaching with Stanli: models, evidence, and familiar workflows

Stanli runs Stan programs without compiling a separate C++ model. This page
connects the tested teaching collections to their numerical checks, performance
measurements, and R workflows. Start with the [classroom installation guide](teaching.md)
or [Coming from cmdstanr](from-cmdstanr.md).

## What is covered

| Collection | Model coverage | R entry point | Evidence |
| --- | --- | --- | --- |
| Statistical Rethinking, second edition | All 61 `ulam()` call sites in the chapters 4–16 supplement, plus one separately labeled hurdle example. Repeated calls are retained; these are 59 distinct Stan/data pairs in total. | Run the Stan program and processed data produced by `ulam(..., sample = FALSE)`. | [Inventory, provenance, and known failures](../tests/rethinking/README.md); [speedup and numerical comparison report](../output/rethinking-report/rethinking-report.md). |
| brms | 124 generated fixtures spanning regression families, multilevel effects, splines, ordinal models, missing data, correlation structures, and Gaussian processes. This is a tested collection, not every possible brms formula. | `make_stancode()` and `make_standata()`, followed by `stanli_model()`. | [Coverage and numerical caveats](../tests/brms/README.md). |
| Educational Stan lessons | 13 attributed teaching models, covering Bernoulli/binomial, Gaussian, hierarchical, count, hurdle, and generalized Pareto examples. Six use transcribed lesson data and seven use explicitly synthetic data. | Load the fixture's `.stan` and `.json`, or use the same workflow with your own teaching data. | [Sources, data, numerical checks, and sampling tests](../tests/educational/README.md). |

Coverage means the listed source/data combinations have tests. It does not
promise support for untested formula combinations or reliable inference from
an arbitrary short run. The inventories retain failures and explain why they
matter.

## Latest complete sweep (16 September 2026)

Fresh Release build on Apple M3 Ultra, 96 GiB RAM, macOS ARM64. Measured runtime/compiler revision
`6e462c2e` includes the measured performance fixes; [the full appendix](../output/teaching-performance/README.md)
records every one of the 199 fixtures, including failures and capped runs.
The [evidence index](../output/teaching-performance/EVIDENCE.md) links numerical
checks, raw-data retention, and reproduction instructions.

| Collection | Completed in both | Lower Stanli CLI time | Median CLI sampling time ratio (CmdStan/Stanli) | Diagnostic screen clear in both |
| --- | ---: | ---: | ---: | ---: |
| Educational lessons | 13/13 | 12/13 | 1.32× | 10/13 |
| Rethinking (including the supplement) | 62/62 | 62/62 | 1.54× | 50/62 |
| brms | 118/124 | 113/118 | 1.45× | 76/118 |

Each model contributes the median of four single-chain CLI runs, each with
1,000 warmup and 1,000 retained draws. Ratios above one mean less elapsed time
for Stanli. They summarize completed fixtures only, without removing diagnostic
flags. Stanli preparation is included; CmdStan compilation is shown separately
in the appendix. This is fixed-budget runtime, not time to equal inferential accuracy.

Adding the measured CmdStan compilation stages gives a first-fit estimate.
Stanli has the lower estimate for 13/13 educational, 62/62 completed Rethinking,
and 118/118 completed brms comparisons. These are sums of measured stages, not
directly timed four-chain R sessions.

The practical target is CmdStan/Stanli ≥ 0.8 for complete CLI elapsed time.
Of all 199 fixtures, 189 meet it, four fall below it, and six lack a complete
comparison. Failed or capped runs do not pass. Later GP fixes and their
[separate validation](brms-performance.md#earlier-gp-and-inverse-gaussian-results)
are not substituted into this sweep.

The [brms performance and numerics report](brms-performance.md) records later
COM-Poisson support, its 0.838× CmdStan/Stanli sampling ratio, and identical
before/after draws. It also covers the remaining timing gaps, caps, and an
upstream negative-binomial GLM precision limitation. These separate measurements
leave the complete-sweep counts above unchanged.

The four-page [Rethinking report](../output/rethinking-report/rethinking-report.md)
covers all 61 book call sites and the separate hurdle fixture. The
[classroom guide](teaching.md#time-from-a-fresh-r-session-to-the-first-posterior)
also records the separate fresh-R-session measurement.

## Generated models and package backends

The brms and Rethinking routes currently produce a **`stanli_fit`**. They do
not produce a complete `brmsfit` or fitted `ulam` object. Consequently,
package-specific operations such as `brms::conditional_effects()` cannot be
applied directly. Native fit conversion and corpus coverage provide the
foundation for those future backends.

### From brms

Use the same formula, data, family, and priors in both generation calls:

```r
library(stanli)
d <- data.frame(x = seq(-1, 1, length.out = 20),
                y = c(-1.0, -0.6, -0.8, -0.4, -0.5, -0.1, -0.3, 0.1, 0.0, 0.3,
                      0.1, 0.5, 0.4, 0.8, 0.6, 1.0, 0.9, 1.3, 1.1, 1.5))
code <- brms::make_stancode(y ~ x, data = d, family = gaussian())
data <- brms::make_standata(y ~ x, data = d, family = gaussian())
model <- stanli_model(code = code, data = data)
fit <- sample_model(model, chains = 4, seed = 1, refresh = 0)
stanli_diagnose(fit)
```

Parameter names are those generated by brms. Inspect `fit$columns` when
choosing variables for plots. Pass all desired prior/family options to both
`make_*` calls rather than changing the resulting Stan text.

### From Rethinking

In the pinned rethinking 2.42 interface, `sample = FALSE` returns the prepared
model and data without fitting it:

```r
prepared <- rethinking::ulam(
  alist(y ~ dnorm(mu, sigma), mu ~ dnorm(0, 1), sigma ~ dexp(1)),
  data = list(y = c(-0.8, -0.2, 0.1, 0.4, 0.9)), sample = FALSE)
model <- stanli::stanli_model(code = prepared$model, data = prepared$data)
fit <- stanli::sample_model(model, chains = 4, seed = 1, refresh = 0)
stanli::stanli_diagnose(fit)
```

For a book model, preserve its preceding data preparation. The corpus generator
records the exact code-box sequence, package pin, and simulation seeds. Models
that the book deliberately makes difficult can also have poor diagnostics in
Stanli; changing the execution engine does not fix an unidentified model.

## Familiar R workflows

| Task | Stanli expression |
| --- | --- |
| Inspect posterior draws | `draws <- stanli::as_draws_array(fit)` |
| Get cmdstanr's default summary columns | `posterior::summarise_draws(draws)` |
| Get stansummary's table | `summary(fit)` |
| Plot traces and divergences | `bayesplot::mcmc_trace(draws, np = bayesplot::nuts_params(fit))` |
| Use tidybayes | `tidybayes::spread_draws(fit, mu)` when the model declares `mu` |
| Compare predictive accuracy | `loo::loo(fit)` when the model declares pointwise `log_lik` in generated quantities |
| Use RStan draw consumers | `sf <- stanli::as_stanfit(fit)`; then `rstan::extract(sf)` |

`as_stanfit()` constructs the fit in memory and provides native density,
gradient, and transform methods through the retained model. RStan is optional;
conversion compiles no C++ model. Read the [compatibility and persistence
limits](stanfit-compatibility.md), including the Jacobian requirement, restoring
live models after serialization, and unsupported RStan resampling operations.
For argument-by-argument translations, including initialization and optimization,
see [Coming from cmdstanr](from-cmdstanr.md).

## How to read the evidence

**Numerics:** the tests compare log density and every gradient component with
independently recorded CmdStan results at three parameter vectors. The Rethinking
and educational corpora also include output-value references at every point.
The older brms corpus has 367 finite density/gradient reference points and five
recorded domain refusals; 307 points also have output-value references. At finite
points without output references, the checker requires output generation but
does not compare its values. The usual scaled-error gate is `1e-9`. The brms documentation identifies
ill-conditioned Gaussian-process exceptions and the original COM-Poisson
refusal. COM-Poisson support and its later sampling measurements are recorded
separately above; historical exceptions still accompany the frozen sweep.
Some points instead check matching domain refusals: for example, the inverse
Gaussian brms fixture is undefined at all three reference vectors. Agreement
on rejection is not evidence of a matching finite gradient at those points.

**Sampling quality:** examine divergences, tree-depth hits, R-hat, and effective
sample sizes before using elapsed time to compare useful inference. The common
benchmark settings do not reproduce every book model's tuning choices.

**Performance:** the current teaching sweep uses a freshly built runtime and
compiler, six paired gradient trials, and four independent single-chain runs
per engine. Compilation and execution are separate. Each Stanli run has a
limit of three times its matching CmdStan run, with an absolute 900-second
ceiling. A failed or capped seed prevents an aggregate time for that engine;
there is no average of only the surviving seeds. See the
[measurement protocol](benchmark-protocol.md) for boundaries and reproducibility.

The earlier Rethinking m14.11 preparation timeout is fixed:
[issue #372](https://github.com/seantalts/stanli/issues/372). In this sweep its
preparation takes 0.161 seconds and its complete CLI median is 11.21 seconds,
versus 13.09 seconds for already-compiled CmdStan. All 62 Rethinking fixtures
complete all four seeds and have lower Stanli median CLI times. The ordered
regression models m12.5, m12.6 and m12.7 have clear screens in both engines.
m13.6 now completes, but retains diagnostic flags in both.
[#373](https://github.com/seantalts/stanli/issues/373) records the follow-up.

Across all collections, the complete sweep has 193 paired comparisons, one
capped case (`sw_re_negbin`), and five fixtures stopped before sampling.
Four completed fixtures remain below the practical 0.8 target: GEV, asymmetric
Laplace, zero-inflated asymmetric Laplace, and mixture theta. The mixture and
the capped fixture have severe diagnostic flags in both engines.

The brms follow-up is tracked in
[#374](https://github.com/seantalts/stanli/issues/374). After this sweep, native
GP arithmetic fixes in `4db5dca2` resolved all three strict GP discrepancies
at every recorded point on this machine. Separate four-seed runs completed
and exceeded the timing target for all three. Inverse Gaussian also samples
in both engines; its shared reference points are outside its domain. These
four fixtures retain diagnostic flags in both engines. COM-Poisson support and
its sampling cap were subsequently resolved in the brms report above. See the [separate results and source identity](brms-performance.md#earlier-gp-and-inverse-gaussian-results);
they do not change the frozen sweep's counts.
