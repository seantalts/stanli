# Native stanfit compatibility

For RStan-style extraction, summaries, and diagnostics **without RStan**, use
`as_rfit()`. That native S3 interface ships inside Stanli; see the
[R interface guide](../r/README.md#native-r-style-fit-methods-without-rstan).
The `as_stanfit()` conversion documented below remains optional and produces
an actual RStan S4 subclass.

`as_stanfit()` constructs an S4 subclass of RStan's `stanfit` in memory from a
`stanli_fit`. It neither writes nor reads CSV and never compiles a C++ model.
RStan is an optional suggested package. This supplies the stored-fit object
for future backends; it does not itself create a complete `brmsfit` or `ulam`.

## Consumer premise, checked September 2026

- **brms:** CRAN 2.23.0 and GitHub 2.23.2 at `4d270eaf` still convert CmdStan
  output through `read_csv_as_stanfit()` in `.fit_model_cmdstanr()`. The object
  stored in `brmsfit$fit` is a `stanfit`. Direct construction remains the right
  foundation for that backend. See [brms backend implementation](https://github.com/paul-buerkner/brms/blob/4d270eafc4b25127f7ce3c8fb96b87146397d083/R/backends.R).
- **rethinking:** GitHub 2.42 at `ac1b3b2c` now defaults to `rstanout = FALSE`:
  `ulam(cmdstan = TRUE)` stores a `CmdStanMCMC` in its `cstanfit` attribute.
  With `rstanout = TRUE`, it calls `rstan::read_stan_csv()` and stores a
  `stanfit` in the `stanfit` attribute, accessed as `@stanfit`. That attribute
  is not a declared slot in the current class definition. Extraction and
  `precis` use it on the RStan path; `link`, `log_lik`, and `sim` also read that
  path. A future backend should account for the native CmdStan default.
  See [ulam fitting](https://github.com/rmcelreath/rethinking/blob/ac1b3b2cda83f3e14096e2d997a6e30ad109eeee/R/ulam-function.R)
  and [class methods](https://github.com/rmcelreath/rethinking/blob/ac1b3b2cda83f3e14096e2d997a6e30ad109eeee/R/ulam-class.R).

## Construction and limits

The constructor was written independently for stanli's array representation.
No brms or RStan constructor code is copied or adapted. Slot semantics were
checked against installed CRAN RStan 2.32.7 and development source
`f070e44e`: [class declarations](https://github.com/stan-dev/rstan/blob/f070e44ec447b5caf1657f70d6c6e629ab43442c/rstan/rstan/R/AllClass.R),
[CSV import](https://github.com/stan-dev/rstan/blob/f070e44ec447b5caf1657f70d6c6e629ab43442c/rstan/rstan/R/stan_csv.R),
and [fit methods](https://github.com/stan-dev/rstan/blob/f070e44ec447b5caf1657f70d6c6e629ab43442c/rstan/rstan/R/stanfit-class.R).

Each chain holds named parameter vectors, with `lp__` last, plus sampler and
timing attributes. Indexed names recover rectangular dimensions with the first
index varying fastest. Sampling metadata distinguishes unthinned iteration
counts from saved warmup and posterior rows. R's RNG supplies extraction
permutations; it does not alter the stored draws. Initial values and adaptation
text are empty because the fit does not retain the actual initial state or
mass-matrix adaptation text.

### Live model adapter

A `stanli_stanfit` inherits RStan's extraction, summaries, and plotting and
registers its own S4 methods for density, gradients, both parameter transforms,
and the unconstrained parameter count. It passes `is(x, "stanfit")` and
`inherits(x, "stanfit")`; exact class-equality checks may need changes.
The original RStan methods are untouched. Registration is delayed until RStan
loads, with both namespace load orders and fresh-session restoration tested.

The adapter retains the original `stanli_model`. Density calls use stanli's
existing C API, and forward transforms use its write-array path, including TP
and GQ. Inverse transforms use declared parameter names and flatten arrays in
Stan's order; extra TP/GQ entries are ignored as RStan's input context does.
Random generated quantities advance the retained model's RNG. The parameter
count is cached and does not require a live pointer.

The runtime already implements BridgeStan's C ABI. The chosen R adapter uses
the existing stanli handle directly, avoiding a second model constructed from a
BridgeStan manifest or another package dependency. Both interfaces currently
support proportional density with the Jacobian included. RStan's
`adjust_transform = FALSE` is explicitly rejected, as is density evaluation
with a `STANLI_LITE_LP` runtime. Removing the Jacobian correctly would require
a separate compiler/runtime capability; silently ignoring the flag is incorrect.
Native RStan resampling and other direct C++-instance operations remain
unsupported. RStan's LOO moment-matching callbacks use public methods and work
through this adapter with a live model and `cores = 1`.

Serialization preserves draws but clears native pointers. Restoring a saved
fit needs no runtime for extraction. To restore model-dependent methods, create
a model from the same code and data, then use
`as_stanfit(saved_fit, model = model)`. Attachment checks column names and
parameter count, and does not mutate other copies of the fit. These checks do
not prove that the original data was supplied. There is no automatic model
compilation, fake Rcpp pointer, or RStan fork.

[CRAN's RStan page](https://CRAN.R-project.org/package=rstan)
provides 2.32.7 binaries for supported macOS and Windows R versions, verified
against the R 4.6 binary indexes. Install the binary to avoid a toolchain;
Linux deployments need a compatible preinstalled binary. No model compiler is
needed for conversion or the supported consumers.

## Independent oracle and validation

`test-stanfit.R` invokes `stanli_run` with the same model, configuration, and
seed. The CLI currently emits one header and concatenated chain rows, without
CmdStan configuration comments. The test splits those rows and adds invocation
metadata before calling `rstan::read_stan_csv()`. It never rewrites numeric
text or obtains reference values from the candidate fit.

The native result preserves every original double exactly, including saved
warmup and `lp__`. Decimal parsing can change the final bit: on the tested
R 4.6.1/macOS build even `as.numeric(sprintf("%.17g", x))` differs from `x`
for some doubles. Thus exact binary equality to CSV import is not promised:
the oracle bounds each draw within two machine epsilons relative to its
reference value. Names, dimensions, and permutations must match exactly;
summaries use floating-point tolerance. The native result is never rounded
to imitate CSV parsing.

Runtime tests cover saved/discarded warmup, thinning, scalar/vector/matrix and
nested array shapes, simplex output, generated quantities, sampler statistics,
timing, serialization, safe native-method failures, RStan and bayesplot plots,
LOO, and ShinyStan. The parser also runs without a runtime. The wheels workflow
supplies an independently built CLI and fails if an ecosystem test skips.

### Numerical and consumer checks for live methods

`tools/record_r_stanfit_reference.R` is a developer-only generator that compiles
one model with RStan and records the results. It never loads stanli. The
committed fixture uses RStan 2.32.7, StanHeaders 2.39.1, and Stan 2.39.0; package
and CI tests read these results without compiling a C++ model. Three parameter
points cover scalars, positive/bounded/ordered parameters, a simplex, a
correlation matrix, matrices, arrays of vectors and matrices, TP, and GQ.
Floating results use a 1e-10 tolerance for printed precision and platform
rounding; names and shapes must match exactly. Unsupported flags, malformed
inputs, missing/serialized models, attachment, and runtime constant conventions
have explicit tests.

A normal-normal model with an influential observation exercises actual LOO
moment matching, comparing its ELPD to the analytic leave-one-out predictive
density within 0.2 log units (Monte Carlo tolerance). This is separate from the
floating-point oracle. The live methods also run under the existing no-skips
runtime CI gate, alongside extraction, bayesplot, RStan, and ShinyStan consumers.

### Recorded validation (16 September 2026)

The Release runtime at `6e462c2e` passes the ecosystem acceptance suite with no
skips, warnings, or failures, including native calls, the independent CSV
oracle, plotting, LOO, and namespace-registration tests. Both generated-model
guide examples run with clear diagnostics. All 249 runtime tests and the 13
educational numerical/output and sampling checks pass.

[Full platform CI](https://github.com/seantalts/stanli/actions/runs/35060736527)
at `6e462c2e` uses the measured runtime/compiler sources and passes Linux, macOS and
Windows R acceptance, AddressSanitizer, ThreadSanitizer, and the full compiler
comparison. Package-only R checks remain separate from runtime acceptance.
Earlier runtime-free `R CMD check --as-cran` reported only the New submission
NOTE; the constructor/parser checks require neither a runtime nor RStan.

On macOS ARM64 / R 4.6.1, five alternating batches of 20,000 evaluations on
the 29-parameter reference model gave a median 3.75 microseconds per direct
`log_prob_grad()` call (range 3.75–4.20), versus 13.25 through
`rstan::log_prob(..., gradient = TRUE)` (12.50–13.65). Each arm warmed for at
least 200 ms. This measures R dispatch and validation overhead on a small
model, not inference speed. [Raw measurements](../output/teaching-performance/native-call-benchmark.csv)
were taken after the full sampling sweep. Ordinary stanli startup still leaves
the RStan namespace unloaded.
