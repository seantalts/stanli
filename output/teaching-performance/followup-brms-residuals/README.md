# Remaining brms sampling and numerical gaps

A separate five-model survey on `cb58eb22`, using the same production executable
as the [COM-Poisson confirmation](../followup-com-poisson-80/README.md).
The Linux, Windows compiler, browser, and R runtime CI checks passed on that
commit. This survey does not replace any observation in the original 199-model
report. No runtime or Stan Math code was changed in this pass.

## Complete-process sampling

Ratios are **CmdStan / Stanli**; above one means Stanli is faster. These are
ratios of the four seed medians, with 1,000 warmup iterations and 1,000 retained
draws per seed. Each Stanli process keeps the original
min(3 × same-seed CmdStan duration, 900 seconds) cap. Source compilation is
included for Stanli; CmdStan model compilation is excluded. One no-argument
launch precedes measurements, so these are warm-executable CLI measurements.
All measurements ran serially, without builds, profiling, diagnostics, or
compression running alongside them.

| Model | CmdStan / Stanli | Completed seeds, Stanli / CmdStan |
| --- | ---: | ---: |
| Negative binomial with group effects | — | 3/4 / 4/4 |
| Mixture with estimated proportions | 0.796× | 4/4 / 4/4 |
| Generalized extreme value (GEV) | — | 3/4 / 4/4 |
| Asymmetric Laplace | 0.753× | 4/4 / 4/4 |
| Zero-inflated asymmetric Laplace | 0.817× | 4/4 / 4/4 |

The negative-binomial seed 2 and GEV seed 1 hit their caps. No aggregate ratio
is calculated from their surviving seeds. This is one prespecified survey,
not a repeated confirmation that a model consistently exceeds 0.8. The
[summary](summary.json) retains seconds, ranges, and median absolute deviations;
[raw sampling evidence](sampling-raw.tar.gz) includes every command, timeout,
CSV, and log.

## Why the negative-binomial reference finishes so quickly

The fast CmdStan seed has **982 divergences in 1,000 retained draws** and
averages **19.78 leapfrog steps per draw**. Its other three seeds average
809–972 steps. Those three completed Stanli seeds average 890–933 steps and
also have divergences and maximum-depth hits. The capped seed produced no
retained draws. These are differences in sampling work as well as runtime;
the short reference duration is not evidence of accurate posterior inference.

Both engines visit extremely large dispersion values and report enormous
positive log densities with tiny adapted step sizes. A standalone reproducer
using only the pinned upstream Stan Math library, with no Stanli headers or
libraries, demonstrates loss of precision in `neg_binomial_2_log_glm_lpmf` in
this regime. It compares 40 observations equal to 3, each with mean 3, against
the equivalent `neg_binomial_2_log_lpmf` call and an independent calculation
using the finite product for the gamma ratio at integer counts.

| Dispersion | GLM log probability | Non-GLM log probability | Absolute difference |
| --- | ---: | ---: | ---: |
| 10⁴ | −59.84290333 | −59.84290333 | 4.27e−10 |
| 10⁸ | −59.83695984 | −59.83690473 | 5.51e−5 |
| 10¹² | −60.25 | −59.83690413 | 0.4131 |
| 10¹⁶ | 6144 | −59.83690413 | 6203.84 |
| 10²² | 17179869184 | −59.83690413 | 1.72e10 |

The GLM subtracts very large terms that should nearly cancel. The equivalent
non-GLM call and independent finite-product calculation agree within
2.1e−12 over all seven test points, including dispersion 10¹⁰⁰. This demonstrates
a numerical limitation of the pinned library in a regime visited by the
chains; it does not identify the cause of every divergent proposal or explain
every difference between the chains.

The [source](negbin-upstream.cpp), [full-precision results](negbin-upstream.csv),
and compiler command are retained. The GLM header is byte-for-byte identical
to its upstream revision. The older adjoint ODE initialization patch recorded
in the [COM-Poisson provenance](../followup-com-poisson-80/provenance.json) is
unrelated. No dependency patch, alternate density, sampler change, or input
change was made to hide this result.

## Remaining runtime work

GEV's three completed Stanli chains and all four CmdStan chains have zero
divergences and zero maximum-depth hits. A separate fixed-point profile puts
**92.6% of gradient evaluation time in the retained loop executor**, with
about 1 ms of model preparation. The sampled call stacks identify control-tree
dispatch, value bookkeeping, and the wrapper around Stan Math's `lmultiply`
callback as candidates for investigation. These are gradient diagnostics,
not additional end-to-end timing observations.

Both Laplace fits have zero divergences and maximum-depth hits in both engines;
all reported parameter R-hats are below 1.01. The mixture has 64 divergences
for Stanli and 1,333 for CmdStan, with poor mixing in both. Its runtime ratio
therefore also needs to be read alongside the sampling work.

[Per-chain work](chain-work.json) and [posterior diagnostics](diagnostics.json)
are retained, including incomplete fits. Original numerical reference results
for this executable remain in the COM-Poisson report; this pass makes no new
claim about full-model log-density or gradient parity.

## Reproduction

[Provenance](provenance.json) records the source revision, dependency identity,
and relation to the previously validated executable. The
[reproduction archive](reproduce.tar.gz) contains the survey and analysis
scripts, the frozen input models and data, the standalone upstream reproducer,
and commands. [Profiling records](profiles.tar.gz) are separate from all timing.
`SHA256SUMS` covers the published files.
