# Models from brms

brms writes a large share of the Stan code that people actually run, and
its output is a corpus posteriordb does not contain: `lprior`
accumulation, the `scale_r_cor` helper, `r_1_1[J_1[n]] * Z_1_1[n]`
indexing, spline and monotonic blocks, the inlined ordinal lpmfs, the
`mi()` imputation shapes. posteriordb carries one brms model, `diamonds`,
from brms 2.10.0.

Every `.stan` and `.json` here is `make_stancode` and `make_standata`
output from brms 2.23.0, written out unchanged.
[`tools/gen_brms_models.R`](../../tools/gen_brms_models.R) is the
generator; it needs brms, mgcv, mice and splines2, and rerunning it
reproduces this directory byte for byte.

These models go through the same oracle as the corpus: CmdStan's recorded
log density and full gradient in `docs/corpus-refs.json.gz`, replayed by
[`tools/verify_refs.py`](../../tools/verify_refs.py) in CI on every push.
The shared [inventory](../../tools/corpus_inventory.py) resolves model names,
source/data paths and provenance for numerical replay and benchmarks.

## What they cover

The `i319_` and `i320_` models are the ones reported in issues #319 and
#320, with the neighbouring variants that isolate what each report turns
on: a Poisson fit with and without a group effect, a truncation with and
without a lower bound, an ordinal fit with and without `cs()`.

The `sw_` models are a sweep over what brms generates elsewhere: the
response families (gaussian, student, skew normal, ex-Gaussian,
asymmetric Laplace, lognormal, gamma, Weibull, beta, von Mises,
Bernoulli, binomial, Poisson, negative binomial, their zero-inflated and
hurdle forms, the four ordinal families, categorical and mixture), the
effect structures (varying intercepts, varying slopes, monotonic effects,
measurement error, missing-data imputation, `s()` and `t2()` splines,
Gaussian processes, autocorrelation terms, multivariate responses with
and without residual correlation, distributional and nonlinear
formulas), and the addition terms (`trunc`, `cens`, `weights`, `se`,
`trials`).

brms emits the same code for `y ~ x + (1 | g)`, for the same formula
under `family = gaussian()`, and for `gr(g, cor = FALSE)` on a single
term, so `sw_re_gauss` stands for all three.

The `s2_` models are a second sweep over what the first one left out.
The families are the ones with a written-out lpdf or a helper of their
own: Dirichlet, logistic normal, multinomial, beta binomial, the
zero-inflated and zero-one-inflated beta forms, zero-inflated asymmetric
Laplace, shifted lognormal, Wiener, Cox with its spline baseline hazard,
discrete Weibull, COM-Poisson, Frechet, generalized extreme value,
inverse Gaussian, hurdle cumulative, hurdle negative binomial, the
probit, cloglog and cauchit cumulative links, and a categorical fit with
a group effect. The effect structures are multimembership with and
without weights and with `mmc()` covariates, `gr(by = )` and
`gr(dist = "student")`, a group effect shared across two responses, and a
distributional `sigma` with its own group effect. The spatial and
autocorrelation terms are `car()` in its three types, `sar()` in both,
`fcor()`, `unstr()`, `cosy()` and `ar(cov = TRUE)`. The addition terms
are interval censoring, `weights()` with `trunc()`, `rate()`,
`subset()` on a multivariate formula, `index()` with `mi()`, and `mi()`
under a lower truncation and under a lognormal response. The rest are
custom families with `vint()` and `vreal()` and their `stanvar`
functions, nonlinear formulas with `loop = FALSE` and with `nlf()`,
cyclic and by-group smooths, `t2(by = )`, the approximate and grouped
Gaussian processes, a Dirichlet prior on a monotonic simplex, two `me()`
terms with and without their correlation, a mixture with a predicted
mixing proportion, and a model compiled for within-chain threading.

Three cases are left out of the generator. `sparse = TRUE` makes brms
write `int vX[size(csr_extract_v(X))]`, the array syntax stanc3 removed;
a nonlinear formula with `loop = FALSE` whose parameters multiply a
covariate makes it write `nlp_b * C_1` with both sides vectors, which
stanc3 rejects on types; and `te()` is not implemented in brms. Nothing
stanli does is involved in any of the three.

## The data

`make_standata` output is written as JSON by the generator, and its
values are kept as they come. Six models carry non-finite ones:
`i320_pois_trunc_ub` truncates from above only and brms fills the lower
bound with `-Infinity`, and `i320_mi_nhanes`, `sw_mi`, `s2_index_mi`,
`s2_mi_trunc_lb` and `s2_mi_lognormal` carry `Infinity` for the responses
they impute and for the imputation bounds. Both parsers read those
tokens.

## Known gaps

All 124 fixtures compile and evaluate at their recorded reference points,
subject to the numerical and domain policies in
[`tools/verify_refs.py`](../../tools/verify_refs.py). This includes COM-Poisson:
its parameter-dependent loop can write vector elements and reduce a prefix
whose length is determined at runtime. The register program checks the indices
and uses Stan Math autodiff replay for those operations. The original failure
and reduced examples are recorded in [#376](https://github.com/seantalts/stanli/issues/376).

Numerical coverage does not imply matching sampling performance. The
[performance report](../../notes/performance/2026-09-16-brms-performance.md) retains the separately
measured performance gaps and sampler measurements.

## Regenerating and recording

```
Rscript tools/gen_brms_models.R
python3 tools/verify_sample.py deps/cmdstan deps/posteriordb sw_gaussian
```

The recorder stamps the file with the revision of every checkout under
`deps/`, `deps/stanc3-src` included, and refuses to merge into a file
recorded against a different one. A tree without that checkout reads its
revision as `unknown` and the merge is refused for drift that is not
there.

The recorder prints one line per evaluation point and writes a reference
at each of them; commit `docs/corpus-refs.json.gz` and
`docs/verification.json` with the model. A point CmdStan refuses is
recorded as a refusal, and one where stanli disagrees is recorded anyway,
because references describe CmdStan. Read the per-point lines before
committing: a `MISMATCH` on a new model is a finding.

## What these found

Earlier recordings marked `sw_gp` and `i320_gp_expquad` as `MISMATCH`
at two of their three points. Their log density and every gradient were bitwise identical
to CmdStan except for the two GP hyperparameters, `sdgp` and `lscale`,
which differed by 1.2e-8 and 2.8e-9 relative. Both flow through
`cholesky_decompose` of the exponentiated-quadratic covariance, which
brms holds up with a 1e-12 jitter on the diagonal. The smallest Cholesky
pivot of that covariance is 1.1e-12 for `sw_gp` and 3.7e-12 for
`i320_gp_expquad` against a diagonal of 1, so the factorization is
singular to machine precision and its derivative divides by that pivot.
This is the same amplification `kronecker_gp` shows in the posteriordb
corpus.

The third point sets every unconstrained value to zero, which zeroes the
latent GP variables and with them the `sdgp` and `lscale` adjoints, so
both models recorded it clean on the arm64 machine those references came
from. The amplification lands in the latent-GP gradients there instead:
moving the GP covariates by one ulp on arm64 moves those gradients by
1.9e-7 and 4.7e-8 relative, and the x86_64 CI runner measures 1.03e-7 and
6.38e-9 against the arm64 references (CI run 33938697559). Which points
come out clean is therefore a property of the recording machine, so both
models are listed in `ILL_CONDITIONED` in
[`tools/verify_refs.py`](../../tools/verify_refs.py) and every one of
their points is held to the limit a `MISMATCH` point gets.

`s2_gp_by_gr` is the third. `gp(x, by = g, gr = TRUE)` builds one
covariance per level of `g`, five of them over eight distinct covariate
values each, and factors each one the same way. Its smallest pivot is
1.8e-12, and its `sdgp` and `lscale` gradients previously deviated by up to 4.15e-7
at the first point and 4.66e-8 at the second. It is listed alongside the
other two.

The CmdStan 2.40 refresh verifies all three models at all three points on
the recording machine. `sw_gp` and `i320_gp_expquad` match bitwise;
`s2_gp_by_gr` stays within 4.44e-16 scaled error. Their documented
cross-platform limits remain in place because the covariance conditioning
has not changed.

`s2_ar_cov` found a live bug. `ar(tt, g, cov = TRUE)` builds the AR(1)
correlation factor from `pow(ar, i - 1)`, and the second and third
evaluation points put the unconstrained `ar` at zero, where stanli
reported a zero derivative for `pow` and CmdStan reported
-4.1658099670210404 and -4.1935902986959563. The log density is bitwise
identical at both points and the other three gradients are within one
ulp. Two lines reproduce it without brms:

```stan
parameters { real a; }
model { target += pow(a, 1); }
```

which stanli evaluated with a gradient of 0 at `a = 0` where the answer
is 1. stan-math's reverse-mode `pow` sends a data exponent of 1, -1, -2
or -0.5 to the base itself, `inv`, `inv_square` or `inv_sqrt` before it
reaches its zero-base guard, so those four carry a partial the guard
drops. Both points were quarantined with the CmdStan values that settled
them; both match now and `QUARANTINED` is empty.

`s2_invgaussian` is undefined at all three points. Its link is
`inv_sqrt(mu)` over a centred linear predictor, which takes both signs
at every evaluation point, so both engines answer with a row of
not-a-number. The recorder already read that row as a refusal, and the
replay now reads it the same way rather than demanding the `EVAL_FAIL`
spelling.

Five models produced no sampler output when these references were
recorded. brms writes `choose(M, 2)` for the size of a correlated
group-effect block; the compile-time int evaluator did not know
`choose`, so the `write_array` graph could not be built for
`sw_re_slope`, `sw_mv_rescor`, `s2_me2`, `s2_mmc` and `s2_mv_shared_re`,
and the per-draw interpreter it falls back to had no
`lkj_corr_cholesky_lpdf`, so `stanli_run` exited with an empty CSV. The
gradients were right the whole time, which is why the replay stayed
green: a `write_array` that failed left the reference unrecorded rather
than failing the gate. `choose` is an integer the compiler evaluates
now, the per-draw interpreter carries the LKJ densities, and the replay
fails a model that produces no `write_array` row at all, so the blind
spot that hid this is closed with the bug.

The `unstr()` density found a wrong answer in the loop machinery. brms
walks the groups of `normal_time_hom_flex_lpdf` with a `while` and
slices by a loop-carried index, `begin[i:I]` and `Jtime[i, 1:nobs[i]]`.
A slice whose upper bound is loop-carried keeps its declaration's
capacity as storage and leaves the unselected tail at zero, and only
`sum` consulted the live length, so `log_sum_exp` reduced over the zeros
with the values, `max` returned zero, `num_elements` returned the
capacity, and a density over the slice integrated the tail; a fixture on
that shape answered 1.863 where CmdStan answers -0.819. The live length
is an operand of the ops that consume the slice now, and the executor
rewrites their operand lengths from it before each call, forward and
backward. Nothing had to be turned on to reach this: a `while` whose
guard is data lowers as a retained loop by default.
