# Stan's other inference algorithms: fixed_param, Laplace, Pathfinder, ADVI

Status: implemented on branch `adoption/brms-algorithms`. The design below
was written before the code and is kept, with what changed marked; results
and limits follow it.

## Why

brms's `algorithm` argument takes `"sampling"`, `"meanfield"`, `"fullrank"`,
`"pathfinder"`, `"laplace"` and `"fixed_param"`. The `stanli` backend runs
only the first. The runtime already has L-BFGS and single-path Pathfinder
(`runtime/src/estimate.cpp`, `runtime/src/pathfinder.cpp`); R exposes
L-BFGS and uses Pathfinder only to start NUTS.

## Rule

Every algorithm here is Stan's own code, called on stanli's model adapter.
Nothing is reimplemented. Where a service cannot be called, the algorithm is
deferred, not rewritten.

## One model adapter for the services

`ExecutorModel` (`runtime/include/stanli/model_adapter.hpp`) is shaped for
the sampler: a rejected point comes back as `-inf`, and `write_array`
ignores the generator it is handed. Stan's services expect a generated
model, which differs in three ways that change results:

1. A rejected point **throws**. ADVI drops and redraws a Monte Carlo sample
   whose gradient throws; with `-inf` and a zero gradient it would keep it.
2. `write_array` draws generated quantities from the **service's own
   generator**, between the service's other draws. That order is what makes
   a seed mean the same thing as in CmdStan.
3. `write_array(include_gq = false)`, which `random_var_context` calls
   while initializing, must not draw.

So the new entry points use a private subclass, `ServiceModel`
(`runtime/src/service_model.hpp`), that does those three things, takes an
unconstrained starting point through `transform_inits` (as the existing
`PathfinderModel` does), and asks a poll function for an interrupt before
each evaluation. `ExecutorModel` and every existing caller are unchanged, so
NUTS, L-BFGS and the existing single-path entry points keep their numbers.

`propto` and `jacobian` stay ignored, as in `ExecutorModel`: the graph has
the Jacobian folded in and each density's `propto` choice baked in. Two
consequences are stated in the limits below.

## Entry points

| Algorithm | Upstream call | C function |
| --- | --- | --- |
| fixed_param | `stan::services::sample::fixed_param` (single-chain overload), once per chain | `stanli_fixed_param` |
| Laplace | `stan::services::laplace_sample<true>` at a mode from the existing `stanli_optimize` | `stanli_laplace_sample` |
| Pathfinder | `stan::services::pathfinder::pathfinder_lbfgs_multi`, or `pathfinder_lbfgs_single` when `num_paths == 1`, which is CmdStan's own dispatch | `stanli_pathfinder` |
| ADVI | `stan::services::experimental::advi::meanfield` / `fullrank` | `stanli_variational` |

### Multi-path Pathfinder and TBB

`multi.hpp` runs its paths in `tbb::parallel_for`, collects them in a
`tbb::concurrent_vector`, and writes through `concurrent_writer`, which
owns a `tbb::concurrent_bounded_queue`. libstanli does not link TBB (it
stubs the two symbols Stan Math's autodiff needs), which is why
`estimate.hpp` says multi-path does not link.

The paths cannot run concurrently here in any case: they share one model,
and a stanli `Executor` is one evaluation's mutable state. So the one
translation unit that includes `multi.hpp` first includes
`runtime/src/tbb_serial.hpp`, which defines those three TBB names as serial
(or, for the queue, mutex-guarded) equivalents and defines the include
guards of the three real headers so they are skipped. Stan's `multi.hpp`
then compiles unmodified and runs its paths one after another. If the real
headers are reached first the build fails loudly. This is a build
arrangement, not a second implementation; PSIS, the resampling and the draw
order are upstream's.

One choice in the stand-in changes results, and was made after the first
measurements. Stan's loop over paths leaves its chunk of the range on the
first path that fails (`return`, not `continue`), so how TBB happens to chunk
the range decides whether the paths after a failed one run at all. The
stand-in hands over one path per chunk, which is what TBB does when it has
workers to feed and what the service's own "Only m of the n pathfinders
succeeded" message describes. Run as one chunk, which is what TBB does with
a single thread, a failure of the first path ended a four-path run of
`i319_pois_re` with "No pathfinders ran successfully". So with one failing
path stanli can return draws where single-threaded CmdStan would stop.

## C API

In the style of `stanli_optimize`: an options struct with an `_init`
function that fills CmdStan's defaults, caller-allocated output buffers, an
`int` return (0 on success) and a message in `err`. All are additive;
`STANLI_ABI_VERSION` stays 1.

Shared by all four:

- **Draws come back constrained**, one row per draw, in the model's CSV
  column order: `stanli_wa_n_columns(m)` doubles, or `stanli_n_constrained(m)`
  when the model has no generated quantities. This differs from
  `stanli_run_pathfinder`, which returns unconstrained draws for the host to
  constrain: the services draw generated quantities from their own generator
  as they go, so constraining afterwards from another stream would not be
  the run CmdStan makes.
- **Per-draw diagnostics** come back in their own buffers, named as cmdstanr
  names them: `lp` (`lp__`) and `lp_approx` (`lp_approx__`). For ADVI these
  are the service's `log_p__` and `log_g__`; for Laplace, `log_p__` and
  `log_q__`.
- **Starting points** are unconstrained (`init`, or null for uniform within
  `init_radius`), as everywhere else in the ABI.
- **Messages**: `stanli_log_cb(level, text, user)` receives what the service
  logs (0 info, 1 warning, 2 error). ADVI's convergence table and Pathfinder's
  Pareto-k warning arrive this way. Errors are also copied to `err`.
- **Interrupts**: `stanli_sample_poll_cb`, asked on the calling thread about
  every 100 ms from inside the model's evaluations; a nonzero answer stops
  the run, which returns 0 with `*interrupted = 1` and no usable output.

```c
int stanli_fixed_param(m, const stanli_fixed_param_opts*, double* values,
                       double* seconds, log, log_user, poll, poll_user,
                       int* interrupted, char* err, size_t err_len);
```
Options: `seed`, `chains`, `chain_id`, `samples`, `thin`, `init_radius`,
`inits`, `refresh`. `values` holds `chains * ceil(samples / thin)` rows,
chain-major. There are no per-draw diagnostics: `lp__` is 0 by definition.
`seconds` (added during implementation) receives each chain's wall time.

```c
int stanli_laplace_sample(m, const stanli_laplace_opts*, const double* mode,
                          double* values, double* lp, double* lp_approx, ...);
```
Options: `seed`, `draws`, `jacobian` (must be 1), `calculate_lp`. `mode` is
an unconstrained point, normally `stanli_optimize`'s.

```c
int stanli_pathfinder(m, const stanli_pathfinder_opts*, double* values,
                      double* lp, double* lp_approx, double* path,
                      int64_t* n_draws, ...);
int64_t stanli_pathfinder_max_draws(const stanli_pathfinder_opts*);
```
Options: `seed`, `chain_id`, `num_paths`, `num_draws` (per path),
`num_psis_draws`, `num_elbo_draws`, `max_lbfgs_iters`, `history_size`,
`init_alpha`, the five tolerances, `init_radius`, `inits`, `psis_resample`,
`calculate_lp`. The row count depends on the run (a failed path contributes
nothing when resampling is off), so the buffers are sized by
`stanli_pathfinder_max_draws` and `*n_draws` reports what was written.
`path` receives `path__`.

```c
int stanli_variational(m, const stanli_variational_opts*, double* mean,
                       double* values, double* lp, double* lp_approx, ...);
```
Options: `seed`, `chain_id`, `algorithm` (0 meanfield, 1 fullrank), `iter`,
`grad_samples`, `elbo_samples`, `eta`, `adapt_engaged`, `adapt_iter`,
`tol_rel_obj`, `eval_elbo`, `output_samples`, `init_radius`, `init`. `mean`
receives the row Stan writes first, the columns at the mean of the
approximation; `values` the `output_samples` draws.

## R

| Function | Returns |
| --- | --- |
| `sample_model(fixed_param = TRUE)` | `stanli_fit`, `algorithm = "fixed_param"` |
| `laplace_model(model, mode = NULL, ...)` | `stanli_fit`, `algorithm = "laplace"`, plus `mode` |
| `pathfinder_model(model, num_paths = 4, ...)` | `stanli_fit`, `algorithm = "pathfinder"` |
| `variational_model(model, algorithm = "meanfield", ...)` | `stanli_fit`, `algorithm = "meanfield"` or `"fullrank"`, plus `mean` |

Each is a `stanli_fit` with one chain (fixed_param keeps its chains), so
`as_draws_array()`, `summary()` and the posterior methods work as they do
for a NUTS fit. `fit$sampler` carries the per-draw diagnostics that exist
for the algorithm (`lp__`, `lp_approx__`, `path__`; `lp__` and
`accept_stat__` for fixed_param) in place of the seven NUTS columns.
Methods that need NUTS columns (`stanli_diagnose`, `nuts_params`) refuse
other algorithms by name.

`as_stanfit()` builds, for the approximate algorithms, the object brms's
`read_csv_as_stanfit()` builds from a cmdstanr fit, because brms's
post-processing is known to work with that: one chain, no warmup, the
model's variables followed by `lp__` and `lp_approx__` as ordinary columns,
no sampler parameters, and `stan_args[[1]]$method` of `"variational"`,
`"pathfinder"` or `"laplace"`. For fixed_param it builds an MCMC stanfit
with `algorithm = "Fixed_param"` and `accept_stat__` as its only sampler
parameter.

Two departures from the brms reader, made during implementation:

- `sim$iter` is the number of draws and `sim$warmup` is 0, where the reader
  leaves both of length zero for these algorithms. `rstan::vb()` sets them
  the same way, RStan's own `extract()`, `summary()` and `print()` need
  them, and brms reads either form (it only asks whether `sim$iter` has a
  length).
- All three approximations carry `lp_approx__`. The reader drops it for
  Pathfinder and Laplace as a workaround for a cmdstanr naming bug
  (brms #1473), not by design.

## brms

`.fit_model_stanli` dispatches on `algorithm` as `.fit_model_cmdstanr` does:
`iter` is ADVI's maximum iteration count, `chains` is Pathfinder's
`num_paths`, `init`, `seed` and `threads` mean what they mean for sampling,
and `...` reaches the stanli function. For sampling, `control` is
translated (`adapt_delta`, `max_treedepth`); for the other algorithms its
entries are passed on as arguments, as the cmdstanr backend passes them.

## Supported and not

- fixed_param: `thin`, per-chain inits. Not: saving warmup (there is none).
- Laplace: `draws`, `mode`, optimizer `iter`/`init`. Not: `jacobian = FALSE`
  (refused, as in `optimize_model`); CmdStan's `mode` as a JSON file of
  constrained values (pass `unconstrain()`'s result).
- Pathfinder: everything in the options above. Not: saving single paths or
  L-BFGS iterations to files; concurrent paths (they run one after another).
- ADVI: everything in the options above. Not: a diagnostic file (the ELBO
  trace reaches the log callback).

## Known differences from CmdStan to check and record

- ADVI evaluates the ELBO with `log_prob<propto = false>`. stanli's graph
  has one density per model, the one `lp__` reports. For a model written
  with `~` statements that drop constants, stanli's ELBO differs from
  CmdStan's by a constant. Gradients and the step-size search are
  unaffected, but the stopping rule is a relative change in the ELBO, so
  the iteration at which ADVI stops can differ. Models that use `target +=`
  throughout, which is how brms writes them, are not affected. Measured
  below.
- Stan's `log_p__` for ADVI is likewise the `propto = false` density.

## Tests

C++ through the C ABI (`tests/test_capi_algorithms.cpp`), fixed seeds: a
model with a known Gaussian posterior, means and standard deviations within
stated Monte Carlo tolerances for Laplace, Pathfinder and both ADVI
families; fixed parameters equal to the init with generated quantities that
vary; the same output for the same seed; error paths (bad options,
`jacobian = 0`, a mode of the wrong kind, an interrupt). R: testthat, the
same properties plus `as_draws_array()` and `as_stanfit()`. brms: each
algorithm through `brm()`, then `summary()`, `posterior_predict()`,
`as_draws_df()`.

## Results (9 October 2026)

Base `f672c551` (`origin/adoption/brms-support`), Linux x86_64, Clang
Release build. No CmdStan was available, so nothing below is a comparison
with CmdStan output. rstan 2.39.0.9000 (Stan 2.39.0) was, and its `vb()`
runs the same ADVI service on the same generator as stanli's Stan 2.40.

### Status

| Algorithm | Runtime and C API | R | brms `stanli` backend |
| --- | --- | --- | --- |
| fixed_param | done | `sample_model(fixed_param = TRUE)` | done |
| Laplace | done | `laplace_model()` | done |
| Pathfinder, multi-path | done | `pathfinder_model()` | done |
| ADVI meanfield, fullrank | done | `variational_model()` | done |

Nothing was deferred. Python and the browser have no bindings for these.

### What the tests establish

- **The generator is Stan's, in Stan's order** (`tests/test_algorithms.cpp`).
  fixed_param on a model with no parameters reproduces, bit for bit, the
  sequence `normal_rng`, `poisson_rng` gives on `create_rng(seed, chain)`,
  for two chain ids. Laplace on a standard normal reproduces the service's
  `std_normal_rng` stream on `create_rng(seed, 0)` to 1e-6, the error of the
  finite-difference Hessian. A rejected row is NaN and keeps the randomness
  it consumed.
- **Known Gaussian target** (`tests/test_capi_algorithms.cpp`,
  `r/tests/testthat/test-algorithms.R`): means, standard deviations and the
  0.8 correlation within five standard errors for Laplace and unresampled
  Pathfinder (ten for resampled Pathfinder, whose draws repeat); full-rank
  ADVI within 0.15; mean-field ADVI at the conditional standard deviation
  0.6 with no correlation, which is the known error of a diagonal family.
- Same seed, same output; another seed, another output; bad options,
  `jacobian = 0`, a missing mode, a stuck path and an interrupt each end the
  way the header says.

### ADVI against `rstan::vb()`, same seed

`tools/compare_brms_algorithms.R OUT.csv rstan`, three brms models, both
families, seeds 1 to 5: 30 runs, 100 posterior means and 100 posterior
standard deviations. The largest absolute difference between stanli and
rstan is 1.0e-07 in a mean and 2.3e-07 in a standard deviation, which is the
six significant digits of the output file `rstan::vb()` reads its draws back
from. The runs are the same runs, including one full-rank run of
`sw_re_pois` that ends 37 posterior standard deviations from NUTS in both.

`tools/check_advi_against_rstan.R` adds two cases
([output](data/2026-10-09-other-inference-algorithms-advi-against-rstan.txt)):

- A run that fails (`y ~ x + (1 | g)`, mean-field, seed 3): both choose
  `eta = 100`, print the same lower bound at iterations 100 and 200
  (-70697.910, -69214.918), and stop on the same Stan exception. stanli
  reports that exception; `rstan::vb()` then fails on an error of its own
  while reading the unfinished output. The two other ADVI failures in the
  table below also fail in rstan with the same seeds; their traces were not
  compared.
- A model written with `~` statements: stanli's lower bound is about 30.1
  above Stan's (the dropped constants). Two of five seeds stop on the same
  iteration and give the same draws; three stop later in stanli (iteration
  700, 500, 1100 against 300) and so give different, equally valid, fits.
  This is the limit stated above, and it does not arise for brms models.

This establishes that ADVI in stanli is the algorithm in rstan, stream for
stream, for models whose `lp__` has no dropped constants. It says nothing
about whether ADVI is a good approximation.

### Each algorithm against NUTS, seven brms models

`tools/compare_brms_algorithms.R`, formulas and simulated data of
`tools/gen_brms_models.R`, through `brm(backend = "stanli")` with brms's
defaults (ADVI limited to 2000 iterations, four Pathfinder paths, 1000
draws). NUTS is four chains of 1000 draws, one run. Each approximation ran
with seeds 1 to 5. "Mean error" is the largest, over the model's summary
parameters, of the distance between the approximation's posterior mean and
the NUTS mean in NUTS posterior standard deviations; "sd ratio" is the
smallest and largest ratio of posterior standard deviations. Raw rows:
[csv](data/2026-10-09-other-inference-algorithms.csv),
[messages](data/2026-10-09-other-inference-algorithms_notes.csv),
[summary](data/2026-10-09-other-inference-algorithms-summary.txt).
`shape` of `sw_negbinomial` is left out: its NUTS sample mean is 3.7e29.

| model | parameters | algorithm | runs that finished | mean error, median run (worst) | sd ratio, median run | runs with a warning |
|---|---|---|---|---|---|---|
| sw_gaussian | 4 | meanfield | 5 of 5 | 0.60 (0.80) | 0.91 to 1.22 | 0 |
| sw_gaussian | 4 | fullrank | 5 of 5 | 0.15 (0.45) | 0.90 to 1.09 | 0 |
| sw_gaussian | 4 | pathfinder | 5 of 5 | 0.07 (0.09) | 0.93 to 1.00 | 0 |
| sw_gaussian | 4 | laplace | 5 of 5 | 0.44 (0.44) | 0.86 to 0.94 | 0 |
| sw_bernoulli | 3 | meanfield | 5 of 5 | 0.35 (0.42) | 0.94 to 1.02 | 0 |
| sw_bernoulli | 3 | fullrank | 5 of 5 | 0.35 (0.44) | 0.85 to 1.23 | 0 |
| sw_bernoulli | 3 | pathfinder | 5 of 5 | 0.05 (0.08) | 0.96 to 1.02 | 2 |
| sw_bernoulli | 3 | laplace | 5 of 5 | 0.03 (0.04) | 0.92 to 0.96 | 0 |
| sw_poisson | 3 | meanfield | 5 of 5 | 0.66 (0.97) | 0.93 to 1.13 | 0 |
| sw_poisson | 3 | fullrank | 5 of 5 | 0.25 (0.30) | 0.88 to 1.17 | 0 |
| sw_poisson | 3 | pathfinder | 5 of 5 | 0.06 (0.11) | 0.99 to 1.02 | 1 |
| sw_poisson | 3 | laplace | 5 of 5 | 0.08 (0.10) | 0.97 to 1.01 | 0 |
| sw_negbinomial | 3 | meanfield | 5 of 5 | 1.11 (1.23) | 0.61 to 0.70 | 0 |
| sw_negbinomial | 3 | fullrank | 4 of 5 | 0.76 (3.43) | 0.67 to 1.28 | 1 |
| sw_negbinomial | 3 | pathfinder | 5 of 5 | 0.84 (0.84) | 0.64 to 0.73 | 4 |
| sw_negbinomial | 3 | laplace | 5 of 5 | 0.84 (0.88) | 0.64 to 0.68 | 0 |
| sw_re_gauss | 4 | meanfield | 4 of 5 | 0.52 (1.02) | 0.48 to 1.12 | 0 |
| sw_re_gauss | 4 | fullrank | 5 of 5 | 0.43 (1.2e+09) | 0.61 to 1.39 | 1 |
| sw_re_gauss | 4 | pathfinder | 5 of 5 | 0.65 (1.09) | 0.81 to 1.05 | 4 |
| sw_re_gauss | 4 | laplace | 5 of 5 | 12.47 (13.13) | 0.89 to 12.15 | 0 |
| sw_re_pois | 3 | meanfield | 5 of 5 | 0.53 (0.61) | 0.59 to 0.96 | 0 |
| sw_re_pois | 3 | fullrank | 5 of 5 | 0.74 (36.75) | 0.59 to 1.10 | 1 |
| sw_re_pois | 3 | pathfinder | 5 of 5 | 0.25 (0.48) | 0.73 to 1.03 | 5 |
| sw_re_pois | 3 | laplace | 5 of 5 | 16.87 (17.35) | 1.08 to 15.69 | 0 |
| i319_pois_re | 6 | meanfield | 5 of 5 | 17.42 (24.25) | 1.03 to 5.87 | 4 |
| i319_pois_re | 6 | fullrank | 4 of 5 | 10.11 (28.52) | 3.09 to 9.03 | 3 |
| i319_pois_re | 6 | pathfinder | 5 of 5 | 3.56 (5.88) | 0.09 to 0.62 | 5 |
| i319_pois_re | 6 | laplace | 4 of 5 | 72.59 (75.41) | 7.96 to 42.16 | 0 |

What this shows:

- On the three models without group effects and with a well-identified
  posterior (`sw_gaussian`, `sw_bernoulli`, `sw_poisson`), Pathfinder and
  Laplace land within about a tenth of a posterior standard deviation of
  NUTS, apart from Laplace on `sw_gaussian` (0.44, the skew of `sigma`),
  with standard deviations within 15%. ADVI is noisier: up to one posterior
  standard deviation off in a single run.
- On the models with group effects the approximations are poor, as the
  methods are known to be for hierarchical posteriors. Laplace is unusable
  there: the mode sits where the group standard deviation is small, and the
  normal built on it is 12 to 75 posterior standard deviations off with
  standard deviations up to 42 times too large. Pathfinder on
  `i319_pois_re` (59 group levels) returns 18 to 45 distinct draws out of
  1000 and warns with a Pareto k above 2 in every run; ADVI stops at its
  iteration limit in most runs.
- Stan's warnings (the Pareto k, the iteration limit) accompany the bad
  Pathfinder and ADVI runs on `i319_pois_re`. Laplace has no diagnostic and
  gives no warning for its bad runs.

What it does not show: that any of these would match CmdStan draw for draw
(only ADVI was compared with another implementation); how the algorithms
behave on larger or differently shaped models; anything about speed.

The four failed runs end with Stan's or the optimizer's own message: two
"number of dropped evaluations has reached its maximum" and one "All
proposed step-sizes failed" from ADVI, and one "Non-finite gradient" from
L-BFGS before Laplace.

### fixed_param

`tests.brm.R` in the brms branch starts both chains at `b = 0.5`,
`sigma = 2`: every draw of `b_x` is 0.5 and of `sigma` is 2, and
`posterior_predict()` varies (sd above 1). The C API and R tests check the
same with the mean and sd of a generated `normal_rng`, and the stream test
above checks it bit for bit.

### Validation

- Build: no compiler warnings in a full Release build with the touched
  sources forced to recompile.
- CTest: 405 of 405 pass (404 before `test_algorithms` was added, all
  passing), including the new `test_capi_algorithms` and `test_algorithms`.
- `tools/check_no_stdio.sh`: no forbidden symbols, with the two new runtime
  sources checked (they are not on its exemption list).
- `tools/verify_refs.py deps/posteriordb --check build-rel/stanli_check`:
  349 of 352, the three GP models failing, which is this machine's baseline.
  No shared evaluation code changed.
- R: 104 tests in 19 files, 821 expectations pass, 15 skipped (V8,
  tidybayes and shinystan not installed, no CSV oracle), 1 fails:
  `test-stanfit.R:203`, an `elpd_loo` comparison at 1e-12 that also fails,
  with the same numbers, on the base commit with the base library.
  `test-algorithms.R` is 6 tests, 242 expectations.
- brms (`tests/testthat/tests.brm.R`, branch `backend-stanli-algorithms`):
  9 tests, 158 expectations, none failing or skipped.
- `tools/format.sh --check`, `tools/gen_docs.py --check`,
  `tests/test_windows_exports.py`: pass.
- `libstanli.so`, stripped: 43,281,280 bytes before, 43,719,840 after
  (+438,560, 1.0%).

Not run: Windows, macOS and WebAssembly builds (the stand-in for TBB and
the export lists are untested there); sanitizers; the Python package; the
compiler-pipeline benchmark, which this change does not touch.

### Limits and open questions

- **`~` statements and ADVI's stopping rule**, measured above. Removing it
  needs a `propto = false` density, which the graph does not have.
- **A failed Pathfinder path**: stanli continues with the others where
  single-threaded CmdStan stops, because of how Stan's loop treats its
  chunk; see the TBB section.
- **`stanfit` layout for the approximations** follows brms's reader except
  for `sim$iter`, `sim$warmup` and `lp_approx__`, as described under R. It
  was not compared with a `stanfit` that brms read from CmdStan output.
- **`update()` in brms** reuses `sim$chains` and `sim$iter` of the fit when
  the algorithm is unchanged. For these fits that is one chain and the
  number of draws, so an updated Pathfinder fit runs one path and an
  updated ADVI fit is limited to as many iterations as it had draws,
  unless `chains` or `iter` is given. An `rstan::vb()` fit has the same
  numbers in those fields; whether the cmdstanr backend behaves the same
  was not checked.
- **stanr** vendors the runtime without `capi.cpp` and a list of other
  sources. `algorithms.cpp` and `pathfinder_multi.cpp` pass the no-stdio
  check, so they can be vendored or dropped there; nothing in stanr was
  changed or tested.
- Pathfinder's paths run one after another. Running them on separate
  executors is possible (the sampler does it for chains) but would mean
  replacing Stan's multi-path driver, not calling it.
