# Stan's other inference algorithms: fixed_param, Laplace, Pathfinder, ADVI

Status: design, written before the code. Results and limits are appended at
the end once measured.

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
then compiles unmodified and runs its paths one after another, which is what
CmdStan does with one thread. If the real headers are reached first the
build fails loudly. This is a build arrangement, not a second
implementation; PSIS, the resampling and the draw order are upstream's.

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
                       log, log_user, poll, poll_user, int* interrupted,
                       char* err, size_t err_len);
```
Options: `seed`, `chains`, `chain_id`, `samples`, `thin`, `init_radius`,
`inits`. `values` holds `chains * ceil(samples / thin)` rows, chain-major.
There are no per-draw diagnostics: `lp__` is 0 by definition.

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
no sampler parameters, `sim$iter` and `sim$warmup` of length zero, and
`stan_args[[1]]$method` of `"variational"`, `"pathfinder"` or `"laplace"`.
For fixed_param it builds an MCMC stanfit with `algorithm = "fixed_param"`
and `accept_stat__` as its only sampler parameter.

## brms

`.fit_model_stanli` dispatches on `algorithm` as `.fit_model_cmdstanr` does:
`iter` is ADVI's maximum iteration count, `chains` is Pathfinder's
`num_paths`, `init`, `seed` and `threads` mean what they mean for sampling,
and `...` reaches the stanli function. `control` applies to sampling only.

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
  with `~` statements that drop constants, stanli's ELBO sits a constant
  below CmdStan's. Gradients and the step-size search are unaffected, but
  the stopping rule is a relative change in the ELBO, so the iteration at
  which ADVI stops can differ. Models that use `target +=` throughout,
  which is how brms writes them, are not affected.
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
