# Changelog

## Unreleased

### A register program writes every register it reads

stanc3's --O1 inliner can declare a user function's return symbol
under a parameter-dependent branch, so a model shaped like
`cond ? udf(x) : y` produced register programs whose live-out
registers were never written when the branch was not taken. The lucky
outcome was a SIGSEGV in log_prob; the quiet one replayed varis from
an already-recovered nested tape and returned a wrong gradient with
no diagnostic. Register programs now prepend a NaN fill for every
adopted register run, on both the island and the ODE right-hand-side
compilers. The conformance harness also stops classifying a killed
worker as not-implemented: a dead child is a blocking `crashed`
status now, which is how this had been hiding in the nightlies.

### Matrix division is a linear solve again

`B / A` on two matrices in the model block computed elementwise
division: no exception, no NaN, a wrong log density and wrong
gradients. stanc3 spells `mdivide_right` with the ordinary division
operator, and the graph lowering read the operator rather than the
divisor's type. `rv / A` refused to compile and `A \ v` was not lowered
at all.

All four shapes -- matrix/matrix, row_vector/matrix, matrix\vector,
matrix\matrix -- now lower to a pair of graph kernels with adjoints,
keyed on the same rule the MIR interpreter already used: a matrix
divisor is a solve, a scalar divisor is not, and `./` never is. A solve
in generated quantities no longer truncates the graph write_array and
sends the whole section to the per-draw interpreter, which measured
6.5x on a 50-element block (5.78 to 0.89 us/draw).

### Out-of-bounds indices are rejected at compile time

Every index the graph lowering sees is a bind-time constant, but most
index forms never checked it: v[7] on a length-4 vector, a gather by
[1, 9], Y[2:9], M[1:2, 5], and their friends silently read a
neighboring arena slot and produced a wrong log density with no error.
CmdStan rejects all of these at runtime. Every indexing path now
checks its bounds at compile time and names the index and the extent;
the matrix row range also accepts hi < lo as empty now, completing the
empty-range semantics. The interpreter's index paths get the same
named errors in place of a bare std::out_of_range("vector").

### Empty gathers compile

A gather whose index slice is empty, such as `Y[Jevent[1:Nevent]]` with
`Nevent == 0`, failed to compile with "gather index must be int data"
(#133). The length is computed from the data, so whether the model
compiled depended on the data: a survival model with no observed events
in one censoring category refused to build. An empty index now lowers
to a zero-length gather that contributes exactly zero to the density
and the gradient; the guard still rejects an index whose int values
disagree with its shape.

### Empty data arrays and empty ranges

The rest of the #133 family, found by auditing every place a
zero-size value can reach the lowering. An empty JSON array ([], which
is what R's integer(0) serializes to) arrived untyped, so an empty int
data array could not index a gather. A range whose realized bounds
make it empty was rejected on the array path ("array outer range out
of bounds") and, worse, emitted a negative-length slice on the vector
and matrix row-range paths, which read out of bounds and produced a
wrong log density with no error. All three now follow CmdStan's
rvalue semantics: hi < lo is an empty slice whatever the endpoints,
bounds are checked only when a range is nonempty, and an empty slice
contributes exactly nothing.

### Non-integer data for an int variable is a data error

A declared-int variable supplied with non-integer values (JSON 1.0 is
not an int, matching CmdStan's var_context) used to bind as typeless
reals, and the failure surfaced at whatever consumer touched it first,
e.g. "gather index must be int data". It is rejected at data binding
now, as std::invalid_argument naming the variable, the same contract
as the existing dimension check.

## 0.8.1

### Overloaded user-defined functions

Models that overload a user-defined function no longer fail to compile
with "mir: user function argument type mismatch" (#125). stanc3 keeps
every overload under the same function name in the MIR; the reader now
gives each overload a distinct internal name and resolves every call
site to the overload its argument types select.

### Deep array literals no longer reach their slot transposed

A rank-3 or deeper array literal in transformed data recorded a
collapsed shape, so the graph's layout bridge permuted it with the
trailing two extents swapped: wrong log density and gradients, no
error (#122). The interpreter now records the literal's full nested
shape.

### Vector inv_logit is bitwise CmdStan again

The elementwise inv_logit kernel handed Eigen's logistic functor a
contiguous temporary, which selects the packet exp that CmdStan's
strided Matrix-of-var path never uses; it was 1 ulp off at some
inputs, within budget but enough to break bitwise fixtures. It now
runs the scalar body CmdStan runs. The same change fixes a GLM with a
scalar outcome reading past the end of its integer group (#123).

### Operator aliases and the var-tape distribution functions

320 conformance rows move to verified (#124). The named spellings of
the binary operators (add, subtract, multiply, divide, elt_multiply,
elt_divide, squared_distance) now lower in all three dispatch paths,
and constant folding of an integer-typed divide no longer returns the
real quotient. von_mises_{cdf,lcdf,lccdf} and
neg_binomial_2_{lcdf,lccdf} join the nested-var-tape tier beside
wiener and ordered_probit.

## 0.8.0

### Pathfinder, and a NUTS vs Pathfinder comparison

Single-path Pathfinder (Stan's own service, stan/services/pathfinder/)
now runs over the executor's gradient, sharing the CmdStan init stream
with NUTS and WALNUTS so a matched seed is a controlled comparison.
The result carries the draws, the log density along the L-BFGS path
with the ELBO-selected iterate, and a Pareto k-hat computed from the
importance ratios. Multi-path is out of scope: it needs real TBB,
which this build stubs.

The comparison page gets a "NUTS vs Pathfinder" mode. Pathfinder has
no chains, so its column shows the optimization path animating live
per L-BFGS iterate, a histogram on a shared x range with each
sampler's outline overlaid on the other's panel, and a Q-Q plot
against NUTS quantiles. Pathfinder finishes in milliseconds and then
the NUTS side keeps streaming into the shared plots, the Q-Q, and a
per-parameter discrepancy column. k-hat is shown with its honest
caveat: it certifies weight stability, never coverage -- on centered
eight schools the approximation misses the funnel badly while k-hat
stays green, which is exactly the failure the view exists to show.

Standalone Pathfinder mode draws no trace plots: its draws are
importance resamples with no serial order, so a trace would read as
perfect mixing by construction. It shows the live L-BFGS climb (one
line per path, ELBO pick marked) and per-parameter histograms instead.

## 0.7.2

### NUTS and WALNUTS start from the same point

For a matched (seed, chain_id), WALNUTS now initializes at the exact
point run_nuts draws: CmdStan's stream, first draw with a finite log
density and gradient. Init policy belongs to the service layer, not to
any one sampler -- in Stan's stack it lives in
stan::services::util::initialize -- so every sampler this runtime
drives shares one policy, and a NUTS-vs-WALNUTS run on the comparison
page is a controlled comparison: both samplers start from the identical
point, and how each handles a bad one is visible.

This replaces 0.7.1's best-of-16 init selection, which started WALNUTS
from easier points than NUTS and thereby hid walnutpie's sensitivity to
deep-tail inits from the very comparison built to show such things.
That sensitivity is walnutpie's to address and is reported upstream;
the lotka regression test went with it, since it pinned upstream
warmup behavior rather than anything this runtime owns. The
find_reasonable_epsilon step search stays: Stan's service layer runs
the same search for NUTS, so it is shared policy too, and without it a
too-large starting step deadlocks WALNUTS outright.

## 0.7.1

### WALNUTS chains no longer freeze on stiff posteriors

On lotka_volterra the comparison page showed a WALNUTS chain drawn as a
flat line: warmup had trapped it at lp ~ -8000 against a posterior
living near -12. Two causes, both at initialization, both fixed in how
stanli supplies walnutpie's caller-provided inputs; the warmup and
sampling algorithms themselves are untouched. First, a uniform(-2, 2)
init can land so deep in the tail that walnutpie's mass adaptation --
which starts learning the metric from its first observation -- collapses
the inverse mass to the tail's huge gradients, leaving a chain that
crawls there for the whole run; WALNUTS inits now take the best log
density among the first 16 finite candidates instead of the first one.
Second, a starting step far too large for the init's neighborhood means
every trajectory extension fails outright (unlike NUTS, which still
moves off a partial tree), so the chain deadlocks while the step only
shrinks about a percent per iteration; the step now starts from Stan's
find_reasonable_epsilon search on the unit metric. Across eight seeds on
lotka_volterra the old behavior froze one chain and left two others
outside the typical set; now all eight sample, with a lotka regression
test pinning the freeze. The underlying warmup sensitivity is being
reported to walnutpie upstream.

## 0.7.0

### A DataMap from a Stan var_context

`DataMap::from_var_context` builds the runtime's data map straight from
a `stan::io::var_context`, for bindings that already map their host
arrays onto `stan::io` and would otherwise have to serialize to JSON
and parse it back out. Reals and integers come from the context's two
separate name lists, and since a var_context already stores
multidimensional values flat and column-major -- the layout an entry
wants -- nothing is reordered on the way in.

Each construction path is its own translation unit, `data.cpp` for the
JSON pair and `data_var_context.cpp` for this one, so a build can take
either without the other; the header declares `stan::io::var_context`
rather than including it, which keeps the stan headers off the include
path of callers that never build a map this way. Requested by
@andrjohns for `stanr` (#81), and built on the branch attached to that
issue.

## 0.6.1

### stanli models behind the BridgeStan ABI

The runtime library now implements the BridgeStan C ABI, so samplers
that drive BridgeStan models -- walnutpie, nutpie, anything using the
`bridgestan` packages -- can drive a stanli model, with no C++
toolchain anywhere. The model travels inside the data argument:
`stanli.bridgestan_model(stan_file=..., data=...)` compiles the model,
splices its manifest under the reserved key `__stanli` (never a data
variable; Stan identifiers begin with a letter), and returns a
`bridgestan.StanModel` bound to the runtime library itself.
`bs_model_construct` validates the manifest, strips the key, and binds
the rest as the model's data. One library serves every model, nothing
is copied, and nothing touches disk.

Every call either behaves exactly as the BridgeStan header documents
or refuses with a message; a differential conformance harness
(`tools/bs_conformance.py`) checks the facade against a real
BridgeStan build in CI. An earlier design that wrote a per-model copy
of the runtime next to a manifest file was built and then removed
before ever reaching a release; the embedded form replaced it because
it needs no ~29 MB copy per model, no `dladdr` (so it works on Windows
and under a plain link), and no cache directory. The one thing the
copy could do that this cannot: serve a client that accepts only a
library path and will not let the caller touch the data argument.
Design notes: `docs/superpowers/specs/2026-08-10-bridgestan-facade-design.md`
and `docs/superpowers/specs/2026-08-11-embedded-mir-data.md`.

### WALNUTS, next to NUTS

The WALNUTS sampler (within-orbit adaptive step-length NUTS,
arXiv:2506.18746) is now built in, via the vendored walnutpie headers
(`runtime/third_party/walnutpie/`, MIT). `run_walnuts` mirrors
`run_nuts` over the same executor gradient, `stanli_sample_walnuts_stream`
mirrors `stanli_sample_stream` on the C ABI, and the npm package's
`sample()` takes `sampler: "walnuts"`. Where NUTS picks one step size
per chain, WALNUTS halves the step within a trajectory wherever the
local error demands it, which is what makes funnel-like geometry
tractable without cranking `delta`; its tunable is `max_error`, the
largest drift in the joint log density allowed across one macro step.

A NaN log density no longer kills WALNUTS warmup. stanli's kernels
report out-of-support points as NaN rather than throwing (log of a
negative is just a NaN), and walnutpie's acceptance statistic fed that
NaN straight into its Adam step-size estimate, where one NaN is
permanent; models whose trajectories cross out of support, dogs_log
among them, died at the end of warmup with "macro_time must be in
(0, inf)". The gradient wrapper now reports non-finite densities the
way walnutpie's own exception path does: -inf, zero gradient.

The browser page grew a sampler picker with a comparison mode: NUTS
and WALNUTS run at once on the same model, data, and seed, NUTS's
column on the left and WALNUTS's on the right, each with live traces,
a histogram, per-chain timing with min ESS and ESS per second, and the
full summary table. Clicking a parameter row in either table selects
it in both, redrawing the traces on a shared y range and the
histograms on a shared x range, so the eye compares mixing and the
posterior rather than axis choices. The live traces share their y
range the same way while the chains run, the two samplers' chains
launch interleaved so a small worker pool cannot serialize them, and
the up and down arrow keys step the selected parameter. Live repaints
track the plotted range incrementally and draw at most two points per
pixel column; both rescans used to pin the page on runs of a few
hundred thousand draws.

Groundwork for letting external samplers drive stanli models, plus two
fixes that stand on their own.

### The generated-quantities RNG belongs to the caller

`WaInterp` owned its random stream and `stanli_wa_seed` reached
through the model to reseed it, so everything drawing from one model
shared one stream. `WaInterp::eval` now takes a stream the caller
owns. The C ABI is unchanged: `stanli_wa_seed` and `stanli_wa_row`
still name one stream per model, so Python, R and the browser see no
difference.

Two fixes fall out. Column discovery at model construction drew from
the caller's stream and now uses a scratch one, and the model's stream
starts from a fixed seed, so `optimize` -- which asks for a row
without ever seeding -- reports the same generated quantities every
run instead of whatever discovery happened to leave behind.

### print() goes where the host says

A model's `print()` was written straight to stdout from two places,
so a program embedding the runtime could not redirect it, interleave
it with its own output, or drop it. Both paths now go through one
sink. The default writes the same line to stdout, and
`stanli::set_message_sink` replaces it; installing and emitting are
serialized, so concurrent chains cannot interleave halves of two
lines.

### Two C API entries

`stanli_stan_to_mir` compiles Stan source to transformed-MIR text
without building a model, so a caller can compile once and keep the
result: cache it, ship it, hand it to another process. Python takes
it back through `Model(mir=...)`, and `stanli.stan_to_mir` wraps it.

`stanli_build_id` names the runtime binary, source revision plus the
build choices that change what that source produces, so anything
cached beside a particular library can refuse a mismatch instead of
silently reading an artifact a different build wrote.

## 0.6.0

Islands stopped paying CmdStan's price for their gradients, the corpus
grew a second half that covers the language rather than the posteriors,
and that second half immediately found three bugs.

### An island generates its backward instead of replaying it

A recurrence is irreducible scalar residue: the re-rolling pass cannot
vectorize it, so the island pass compiles the region into one
register-machine op. For most of that pass's life the op collapse was
dramatic and the time followed on exactly one model, because the
backward re-executed the whole program under `stan::math::var` -- a vari
per operation, a virtual `chain()` per operation, a nested tape built and
torn down per call. Correct by construction, and it costs what CmdStan
costs.

- **`gen_adjoint` (`runtime/src/adjoint.cpp`)** differentiates the
  forward program into a second register program: reverse-mode source
  transformation over the ~35 opcodes of `Program`, running on doubles
  with no vari, no nested tape and no allocation. Each rule is the
  corresponding stan-math rev expression transcribed with the same
  grouping, because the bar is bitwise agreement with the replay rather
  than a correct derivative. `STANLI_NO_NATIVE_ADJ=1` restores the
  replay, which is the oracle it is tested against.
- **Measured** against `STANLI_NO_ISLAND=1` on all twenty-one corpus
  models that compile a region: `iohmm_reg` **4.74x**, `hmm_gaussian`
  1.60x, `hmm_example` 1.56x, `hmm_drive_1` 1.41x, `hmm_drive_0` 1.39x,
  `garch11` 1.36x. The recurrence slice used to sit at 0.6-0.9x against
  CmdStan and now crosses parity.
- **The register machine speaks the graph's whole vocabulary.** It had
  50 opcodes against the graph's 294, and one op outside that set ended a
  region (`POW` used to split them in half). A `CALL` instruction now
  runs the graph's own kernel over a register range -- the identical
  code, partials and backward -- so an unknown op costs continuity
  rather than the region. The carve estimate charges it the graph's
  per-op tax, so no previously carved verdict changed.

### A language corpus beside the posteriordb one

posteriordb is 119 real posteriors, and real posteriors use a small part
of Stan. Nothing in it declares `offset`/`multiplier`, a
`cholesky_factor_cov`, a `sum_to_zero_matrix`, a user `_lupdf`, or an
integer modulus.

- **Ten models lifted from stanc3's own test suite**
  (`tests/stanc3/`), where they exist to be compiled and never run.
  `stanc --debug-generate-data` writes their data, which is what makes
  them usable at all: this lowering evaluates transformed data eagerly,
  so a model without data cannot be lowered. They replay through the
  same CI step as the corpus, which now covers 129 models.
- **write_array references are recorded for every model whose row is
  deterministic**, not only for models with a generated quantities
  block. The parameter columns are the row too, and their order is
  exactly what one of the bugs below got wrong.
- **Every reference re-recorded**, each model at a point inside its own
  support: a point where the density is zero compares -inf against -inf
  and exercises nothing.

### Three bugs it found, all fixed

- **An array of matrices reached the CSV transposed.** An
  `array[N] matrix[R, C]` is array-major outside and column-major
  inside, and one row-major stride walk over all three dims transposes
  every element while leaving the column names right. `log_prob` never
  notices, so this was invisible to every gradient check; the reported
  draw was wrong. The offset arithmetic now lives in one function that
  the read path, the write path and the data repack share.
- **A user density called normalized stayed unnormalized.** `f_lpdf`
  whose body calls `normal_lupdf` must drop the normalizing constant
  only when the caller wrote `f_lupdf`. The MIR reader was not reading
  the propto flag off a user call at all, so `lp__`, and any transformed
  parameter or generated quantity computed that way, was off by the
  constant. Gradients were unaffected, which is how it survived.
- **`sum_to_zero_matrix` had the wrong number of free parameters**, and
  now works. Its read dims are indistinguishable from
  `array[N] sum_to_zero_vector[M]`, and it had been lowered as that:
  `N*(M-1)` unconstrained where Stan has `(N-1)*(M-1)`, since the matrix
  transform centers both axes. It is its own kernel now, bitwise against
  CmdStan on 147 gradients and 630 written values.

### Documentation

- [`docs/hacking.md`](docs/hacking.md) rewritten for readers who know
  Stan and have never worked on a compiler, and
  [`docs/lowering-walkthrough.md`](docs/lowering-walkthrough.md) traces
  three small models -- the vectorized path, a parameter branch, and a
  recurrence with its generated backward -- through every layer.
- A README per runtime directory explaining the header/translation-unit
  split, and `clang-format` gated in CI.

## 0.5.1

A re-release of 0.5.0 for the R distribution channel; no code changes.
The v0.5.0 GitHub release was published before its runtime assets
attached, and under immutable releases that freezes it empty; deleting
it tombstones the tag name forever, so the assets `stanli_install()`
downloads need a release that can carry them. The release workflow now
attaches assets to a draft and publishes last, which is the order
immutable releases require.

## 0.5.0

Three bindings and a workflow. 0.4.x could sample a model from Python or
a browser tab but could not tell you whether the run was any good, and a
2026 model written with `offset`/`multiplier` would not compile at all.
This one runs four chains in parallel with the diagnostics to judge
them, lowers every parameter transform Stan has, finds the mode, adds an
R package, verifies brms-shaped models, and closes with a reviewed
cleanup of the engine all of it sits on.

### Multi-chain sampling and diagnostics

- **`sample()` runs four chains by default, in parallel.** R-hat needs
  more than one chain, so a single-chain default made convergence
  uncheckable. Chain `c` uses CmdStan's stream for `(seed, chain id
  c+1)`, so a matched seed means a matched stream per chain. Eight
  schools does all four in about 70 ms.
- **Threading changes nothing about the answer.** Each chain owns its
  executor and its RNG stream, so the draws are byte-identical to a
  sequential run -- checked in `tests/test_multichain.cpp` and
  `tests/test_python.py`, and on the CLI across four models including one
  that spends 72% of its gradient in a nested var tape.
- **`fit.summary()`**: mean, MCSE, sd, quantiles, bulk and tail ESS, and
  rank-normalized split-R-hat (Vehtari et al. 2021), computed by stan's
  own estimators so the numbers agree with `stansummary`.
- **`fit.diagnose()`**: the checks a workflow actually turns on --
  divergent transitions, max-treedepth saturation, **E-BFMI**, R-hat, and
  bulk/tail ESS -- each either confirmed or reported with the number that
  failed and what to do about it.
- `fit.draws("mu")` keeps the chain axis; `fit["mu"]` concatenates, which
  is what `sample()` returned before, so existing code is unaffected.
  `fit.to_arviz()` hands off an InferenceData with the sampler stats
  attached.
- New sampler controls, in Python, the C ABI and `stanli_run`: `chains`,
  `thin`, `save_warmup`, `inits` (unconstrained), `init_radius`,
  `max_depth`, `parallel_chains`. `stanli_run` gains `--chains`,
  `--num-threads`, `--thin`, `--save-warmup`, `--init-radius` and
  `--summary`.
- `stanli_sample_multi`, `stanli_summary_stats`, `stanli_diagnose_text`
  and `stanli_thread_safe` join the C ABI.

**`STAN_THREADS` is on for native builds, and it was measured before it
was turned on.** stan-math's autodiff stack becomes thread-local, which
costs a TLS indirection on every var operation -- so the model to worry
about is one dominated by the nested-tape path. A 200-step
`ordered_logistic` recurrence spending 72% of its gradient inside a
legacy op measured 44,695 ns without and 44,370 ns with; eight schools
221.4 against 223.0. Noise in both directions, against 5.9x for 8 chains
on 8 threads.

The bug that made this worth doing carefully: stan-math's AD stack
pointer is thread-local under `STAN_THREADS` and starts **null** in every
new thread, so each child thread must instantiate a `ChainableStack`
before touching the AD system. CmdStan never writes that line because
TBB's scheduler-entry hook does it for every worker -- and this build
stubs TBB out. Raw `std::thread`s segfaulted inside `start_nested()`
until `run_nuts_chains` did it itself.

### Optimization

`Model.optimize()` runs L-BFGS -- stan's own, the one behind CmdStan's
`optimize` -- over the same gradient the sampler uses, and returns the
mode as every CSV column plus the unconstrained point. That point is
what `sample(inits=...)` takes, which is the reason to have it.

**It returns the posterior MODE, and refuses CmdStan's default.**
CmdStan's `optimize` defaults to `jacobian=0`, the penalized maximum
likelihood. stanli folds the change-of-variables Jacobian into the graph
at lowering time and the model adapter ignores the template flag
entirely, so `jacobian=False` raises rather than quietly returning the
other quantity under that name -- they differ for any constrained
parameter, which is most models. Excluding the Jacobian is possible in
principle (`lower.cpp` already collects `jac_slots` separately) and is
what the fix would be.

`ExecutorModel` grew the rest of the stan model concept to get here:
`log_prob` in its `std::vector` form as well as its Eigen one,
`constrained_param_names` (which APPENDS -- the services push their own
columns first), `write_array` in both forms, `get_dims`, and a
`transform_inits` that throws, because unconstraining a user's starting
values needs the INVERSE parameter transforms and only the forward ones
exist.

**Pathfinder is not here.** The adapter is now complete enough that
stan's service compiles and runs against it, but the draws come back
empty -- the parameter writer is never called -- and an entry point that
silently returns nothing is worse than none. Multi-path additionally
needs real TBB, which this build stubs out, so its `tbb::parallel_for`
does not link at all.

### The modern ODE interface

`ode_rk45`, `ode_bdf`, `ode_adams`, `ode_ckrk` and their `_tol` forms.
Only the deprecated `integrate_ode_*` family lowered before, so a model
written against the interface Stan has recommended for years did not
compile.

The two differ in more than spelling: the modern right-hand side takes a
`vector` state and returns a `vector`, and everything after `ts` is
passed through in any number and any type, where the old one fixed
exactly `(theta, x_r, x_i)`. Both now reduce to one calling convention --
autodiff reals packed in order, data reals packed in order, integers as
compile-time constants -- so the register machine that made ODE
right-hand sides 29-39x faster is unchanged and serves both.

Each solver dispatches to its own stan-math entry point. Mapping
`ode_adams` onto BDF, or `ode_ckrk` onto RK45, agrees to solver tolerance
on an easy system and is still the wrong integrator for the user who
chose one for its stability -- and it would have passed a casual test.

The interpreter fallback follows: a right-hand side the compiler cannot
take still runs, as it always has, because the spec now carries the
argument list the fallback needs to split the packed arguments back into
the function's declared parameters. Coverage never shrinks, only speed.

Verified against a CmdStan build of the same model at three points, 11
interfaces including the deprecated one, worst 1.1e-14 relative
(`harnesses/ode_sweep.py`). One bug found on the way, and it is the kind
worth naming: the data-argument packing called `const_values(a)` twice
and took `begin()` from one temporary and `end()` from the other. That is
an invalid range and it does not fail loudly -- it appended hundreds of
garbage doubles and surfaced much later as `ode parameters and
data[927] is nan`.

### Parameter transforms, reject and print

- **`offset` / `multiplier`.** The modern non-centering idiom, and what
  brms generates. Its offset and multiplier may themselves be parameters,
  scalar or per-element. It parsed into the MIR before this and then hit
  `unsupported parameter transform` in the lowering.
- **`unit_vector`, `sum_to_zero_vector`, `corr_matrix`, `cov_matrix`,
  `cholesky_factor_cov`** (square and rectangular). That completes the
  set, and it is what lets `lkj_corr` and the wisharts be declared
  directly rather than reached through a transformed parameter.
- **`reject` and `print`**, in both placements: `transformed data`, where
  a taken reject fails the compile the way CmdStan fails to construct the
  model, and the model block, where it lowers to an op that throws
  `std::domain_error` during the forward sweep -- the same exception from
  the same place CmdStan throws it, so the sampler reads it as a rejected
  proposal. A reject under a condition on a *parameter* still does not
  lower, because the condition does not; that is the parameter-dependent
  control flow gap, not a reject gap.
- All 20 transforms verify **bitwise** against a CmdStan build of the
  same model (`harnesses/transform_sweep.py`). One of them took a second
  pass: with a vector multiplier, the adjoint has to accumulate the value
  term and the Jacobian term as two separate `+=` rather than one sum of
  two, because stan-math builds the lp term before the value and the
  reverse sweep contracts them in that order. `a += b; a += c` does not
  round like `a += (b + c)`, and that was the whole of a 1-ULP gap.


### An R package

The same runtime behind an R binding (#33, #35): `stanli_model()`,
`sample_model()`, `summary()`, `stanli_diagnose()`, `optimize_model()`,
and `as_draws_array()` for the posterior ecosystem. Two choices make it
a package CRAN could carry:

- **The Stan compiler is stanc3 compiled to JavaScript**, run through
  the V8 package, rstan's approach: one 2.8 MB file, no toolchain, no
  per-platform binaries. Where the runtime embeds stanc3 that path is
  used instead and V8 never loads; `tests/test_stancjs.cjs` pins the
  JavaScript compiler's MIR byte-for-byte against the native binary.
- **The runtime downloads on first use** with `stanli_install()`, pinned
  to the release the package was built against. Release tags now attach
  the five platform runtime tarballs, so the GitHub release is the R
  package's distribution channel. Because binding and runtime are
  separately versioned artifacts, the C ABI carries a layout version
  (`stanli_abi_version()`) and the bridge refuses a runtime that
  disagrees: reading the options struct at wrong offsets would not
  crash, it would sample from the wrong seed.

Not on CRAN yet; install from
[r-universe](https://seantalts.r-universe.dev) or a checkout
(`r/README.md`).

### brms-shaped models, and a bug only a second evaluation could catch

`harnesses/brms_sweep.py` generates eight brms-shaped models (lprior
accumulation, correlated random effects, splines, monotonic effects, the
ordinal and bernoulli GLMs, posterior-predictive generated quantities)
and verifies each against a CmdStan build; all eight pass, six bitwise
(#32). Two fixes came out of it:

- **The same point evaluated four times gave four different log
  densities.** The in-place pass verified that a vector's zero-fill was
  an op before making `mu[n] +=` writes destructive; constant folding
  then replaced that op with a bind-time fill, and the writes
  accumulated into a buffer nothing reset. Folding now refuses to fold
  away the producer of a slot that a surviving read-modify-write op
  needs restored each evaluation. Nothing structural could have caught
  it, and the corpus rig cannot: it evaluates one point per process, so
  a model that drifts across evaluations verifies perfectly and then
  samples from the wrong posterior.
- **`rows()` in a real-valued expression.** brms's `mo()` helper writes
  `rows(scale) * sum(scale[1:i])` in the middle of arithmetic, and the
  function was answered only where an integer was expected.

### Three shape fixes, from a 0.4.1 that was never tagged

Three bugs in features 0.4.0 introduced. Two refused to compile, one was
silent. All three were found by exercising shapes the posteriordb corpus
does not contain, and all three are covered by fixtures now. (These were
drafted as a 0.4.1 that never shipped; no such version exists on PyPI.)

#### array[N] vector[K] data reached the multivariate densities permuted

`y ~ multi_normal(mu, Sigma)` with `array[N] vector[K] y` as data returned
wrong gradients. The model compiled and `lp__` stayed plausible, so
nothing announced it.

Data is stored with the first index fastest, the way a matrix is stored.
An array of vectors has to reach the kernel with element `n` contiguous in
`K`, which is where a parameter of the same type already sits, so the data
path now repacks on the way into the slot. The same slot indexed one
element at a time was always right, which is why the shape looked healthy.

Affected: `multi_normal`, `multi_normal_cholesky`, `multi_normal_prec`,
`multi_student_t` and `multi_student_t_cholesky`, only with an
`array[N] vector[K]` outcome that is data, and only when the whole array
is passed. All five now match CmdStan at 0 ULP on every gradient. The same
outcome as a parameter was correct before and still is.

No posteriordb model has this shape, so the corpus never covered it.
`tests/fixtures/mnarr.stan` does now, with `N` and `K` deliberately
different, since a square case hides a transpose.

#### Vectorized dirichlet did not compile

`p ~ dirichlet(a)` over an array of simplexes, the shape a hierarchical
Dirichlet is written in, threw at evaluation time. The kernel took one
theta vector and read the whole slot as it, so the vectorized form reached
stan-math as a single simplex of `N*K` and failed the length check against
alpha. Only the explicit `for (n in 1:N) p[n] ~ dirichlet(a)` worked.

The kernel splits the slot now. A single dirichlet needs theta and alpha
the same length, so a longer theta is unambiguously the vectorized form.
Data and parameter outcomes both match CmdStan at 0 ULP.

#### Vectorized truncation did not compile

0.4.0 added truncation and tested it on `real y`, which is the one shape
that compiled. Every vectorized form failed at compile time, and
`y ~ normal(mu, sigma) T[0, 10]` over a vector is the form models are
written in. Two constructs were missing, one per shape stanc3 emits:

- A scalar location gives a normalizer of `FnLength(y) * log_diff_exp(...)`.
  `FnLength` is a compiler-internal rather than a stan-library name, so it
  reached the lowering as an unknown function kind. It answers as
  `num_elements` does, matching `stan::math::size`, which is what stanc3's
  own backend maps it to.
- A container location with a literal scale makes stanc3 loop over the
  elements and hoist the scale into a temporary it declares
  `(Unsized UReal)`. The reader only understood sized declarations, and a
  scalar carries no size expression.

Both are fixed. Five shapes now match CmdStan at 0 ULP on `lp__` and every
gradient: vector outcome, array outcome, one-sided `T[a, ]` and `T[ , b]`,
a vector location, and a truncated `_lpmf`. `tests/fixtures/truncvec.stan`
covers the two that failed, so CI catches this without CmdStan installed.

### The demo page carries the corpus, and npm carries a scope

Every verified posteriordb model (117 of them) is on the
[demo page](https://seantalts.github.io/stanli/) now, searchable, lazily
loaded: the page fetches only an index until a model is selected. The
npm package is `@seantalts/stanli`: npm's name-similarity filter rejects
the unscoped name, and the scope is what lets the tag flow publish.

### Re-rolling is O(n log n) in time, and the test that keeps it there counts

The re-roll pass was linear in memory but quadratic in time: every
region answered its range questions by scanning whole per-slot use lists
(#36). ldaK5 refills one shared 5-slot vector from each of its 33,000
iterations, so those lists are 33,000 entries long and every region
walked all of them: 11.3 billion list entries read, against 3.6 million
for everything else in the pass combined. Binary search asks the same
questions of the same lists and reads only the entries that can matter.

The regression gate then learned its own lesson: two wall-clock
formulations of "still linear" failed on shared CI runners while the
pass was fine. The pass now counts every list entry it reads
(`RerollStats::list_steps`, probes included), and the test asserts on
that exact integer's ratio between two sizes: 2.1x for the shipped pass,
4.0x for the quadratic scan it provably rejects.

### A reviewed simplification of the engine

The docs were rewritten about a third shorter with their stale claims
fixed (#37), and the runtime went through a staged review (#39): survey
reviewers proposed 84 simplifications, adversarial verification refuted
26, and the 57 approved ones landed one commit each, 579 lines removed
net. Nothing changed behavior by every oracle the project has: the
corpus replay's worst-deviation line is byte-identical before and after,
the passes-on/passes-off A/B is byte-identical, and per-gradient
benchmarks across the touched surfaces are flat. Two findings became new
tests: the `STANLI_NO_INPLACE`/`STANLI_NO_ISLAND` kill-switches and
`forward_value_only` had no coverage, and now do. The plan and the
adjudicated findings are committed under `docs/superpowers/plans/`.


## 0.4.0

Coverage. 0.3.0 shipped 46 of Stan's 72 densities and could not compile a
truncated model at all. This one has 71, and truncation and censoring
work.

### Distributions

- **71 of 72 densities.** New since 0.3.0: the count distributions
  (`neg_binomial`, `neg_binomial_2_log`, `beta_neg_binomial`,
  `yule_simon`, `beta_binomial`), the count GLMs (`poisson_log_glm`,
  `neg_binomial_2_log_glm`, `binomial_logit_glm`, `categorical_logit_glm`,
  `ordered_logistic_glm`), ordinal regression (`ordered_logistic`,
  `ordered_probit`), the multivariate tail (`multi_normal_prec`,
  `multi_student_t` and its cholesky form, the wishart family, `multi_gp`
  and its cholesky form, `lkj_corr`, `lkj_cov`), the multinomial family,
  `hypergeometric`, `discrete_range` and `wiener`.

  `gaussian_dlm_obs` is the one that is out, for a structural reason: it
  takes seven arguments and an op holds six.

- **Truncation and censoring.** `y ~ normal(mu, sigma) T[0, 10]` did not
  compile before. stanc3 rewrites a `T[,]` into the density minus
  `log_diff_exp` of the bounds' `lcdf`s, and neither piece existed.

- **90 of 105 distribution functions**, the `cdf`/`lcdf`/`lccdf` family
  that truncation runs on, continuous and count alike. Every one is 0 ULP
  against CmdStan.

- [docs/coverage.md](docs/coverage.md) lists what is still missing and
  what each gap needs. It also opens with the three ways a density gets
  added, cheapest first, because reaching for a kernel first is what kept
  the list short longer than it had to be.

### Two bugs worth naming

- GLM ops were the one density shape the lowering gave no variant at all,
  so their kernels hardcoded `propto=false`. `poisson_log_glm`'s `lp__`
  came out `sum(log(y!))` away from CmdStan's with every gradient already
  exact. `bernoulli_logit_glm`, which shipped in 0.3.0, had the same
  hardcoding and got away with it because bernoulli has no constant to
  drop.

- `Graph::add_op` wrote past `Op::in` with no bounds check. A seven-input
  op corrupted `n_in` and surfaced as a SIGBUS inside a kernel rather than
  at the point that knew. It throws now.

### The reported lp__, and the compact tier

The multivariate and multinomial tail is built compact: one instantiation
of stan-math's template instead of one per activity mask. Gradients and
`write_array` values are bitwise against CmdStan; `lp__` sits a per-model
constant higher, because stan-math's term-dropping is keyed on argument
types and a single instantiation cannot reproduce it.
[docs/compact-densities.md](docs/compact-densities.md) says which
densities are exact, which are compact, and how to make a compact one
exact.

`STANLI_LITE_LP` applies the same trade globally and is off everywhere,
browser included. It defaulted on for the browser during development,
which meant the demo reported an `lp__` that could not be compared against
CmdStan. Every build reports the same `lp__` now.

### Browser

- SIMD128, worth 2% on most shapes and 11% on a matrix-heavy model for
  0.03 MB gzipped. Gradients stay bitwise identical to the scalar build.
- Exact `lp__`, as above. `stanli.wasm` is 5.80 MB raw and 1.52 MB
  gzipped.
- `-ffp-contract=fast` produces a byte-identical binary here: baseline
  WebAssembly has no FMA instruction, so there is nothing to contract.
- Loading uncommon densities from a side module was built and removed.
  [docs/density-pack.md](docs/density-pack.md) records the measurements
  and the one emscripten limitation that blocks it.

### Build and tools

- The density kernels are nine translation units instead of one. That one
  file peaked at 7.6 GB of compiler memory and serialized the build.
- `stanli_run` compiles the model in process when built with the stanc3
  embed object: one binary, `.stan` and `data.json` in, CmdStan-shaped CSV
  out, no toolchain and no separate compiler to find. `cmake --install`
  places it, `stanli_check`, the library and the headers.
- `stanli_check` reports a nonfinite `lp__` or gradient as a value instead
  of refusing. `ref_driver` always did, so the asymmetry meant the oracle
  could never confirm agreement at -inf.
- `tools/bench_wasm.cjs` measures ns/gradient in the browser build.
  `tools/verify_lite.py` checks a `STANLI_LITE_LP` build against the exact
  one. `tools/verify_refs.py --no-lp` replays the corpus for a build whose
  `lp__` is shifted by design.
- CI runs one workflow per branch, cancelling superseded pull-request runs
  but never a push to main or a tag build.

### Sizes

| | 0.3.0 | 0.4.0 |
|---|---:|---:|
| shared library installed | 21.3 MB | 22.2 MB |
| wheel | 7.4 MB | 7.8 MB |
| `libstanli` stripped | 14.93 MB | 15.75 MB |
| `stanli.wasm` gzipped | 0.99 MB | 1.52 MB |

The library grew with the density list. The browser payload grew mostly
because `STANLI_LITE_LP` came off, which bought an `lp__` that matches
CmdStan.

## 0.3.0

The releases in between never shipped: 0.2.1 was written up but never
tagged, so everything below is what changed for anyone upgrading from
0.2.0.

### Two things change results

- **The sampler draws from CmdStan's generator.** It built
  `boost::ecuyer1988` from the seed while CmdStan builds
  `boost::random::mixmax` as `(0, 1, seed, chain)`, so the same seed
  named unrelated streams and any sampling comparison was comparing two
  different draws as much as two engines. `run_nuts` calls
  `stan::services::util::create_rng` and draws the initial point the way
  `stan::io::random_var_context` does. A given seed now produces
  different draws than 0.2.0 did. Any seed is as valid as any other, but
  a run pinned to one will not reproduce byte for byte.

- **The browser build reports a shifted `lp__`.** This is the one
  number in this release that does not match CmdStan, so it is worth
  being precise about what does and does not move.

  The browser runtime is built with `STANLI_LITE_LP`, which drops
  stan-math's propto instantiations. A density is not one function:
  stan-math decides which terms of a log density to keep by looking at
  the argument types, so `y ~ normal(mu, sigma)` with data `sigma`
  drops `-0.5 * log(2*pi)` and is a different instantiation from the
  one that keeps it. Supporting that exactly costs `4 * 2^N` copies of
  the template per distribution, about 630 KB each, which is half the
  library.

  Dropping the propto half means `~` evaluates the full density. The
  terms it stops removing are exactly the ones that are constant in the
  active arguments, so they have no derivative to contribute:

  - Every gradient is bitwise identical to the exact build, measured
    across the whole 119-model corpus.
  - Every `write_array` value, so every constrained parameter,
    transformed parameter, and generated quantity, is bitwise
    identical.
  - The posterior is the same posterior. `lp__` lands a per-model
    constant away from CmdStan's.

  Two consequences. Do not compare a browser `lp__` against a CmdStan
  run, and do not feed it to anything that reads log densities as
  absolute numbers: Bayes factors, marginal likelihoods, bridge
  sampling. And because NUTS adds `lp` to the kinetic energy, a shifted
  `lp` rounds differently there, so a pinned seed draws a different
  chain in the browser than in the wheels. It is an equally valid chain
  from the same posterior, the same class of difference as reseeding.

  **The PyPI wheels are unaffected.** `STANLI_LITE_LP` is on by default
  only for emscripten; every wheel ships the exact build and matches
  CmdStan's `lp__`. `stanli_exact_lp()` in C, `stanli.exact_lp()` in
  Python, and `fit.exactLp` in JS report which build is loaded, and
  `tools/verify_lite.py` is what checks the claims above. Full write-up
  in [docs/lite-lp.md](docs/lite-lp.md).

### Stan in the browser

- stanc3 compiled to JavaScript through its own js_of_ocaml target, this
  runtime compiled to WebAssembly through Emscripten, and nothing on a
  server: a model is compiled and sampled in the page. 118 of the 120
  corpus models replay through the WASM build under Node against the
  same recorded CmdStan values the native build is checked against,
  generated-quantities columns included (`nn_rbm1bJ100` is the
  exception, and it wants more than the 4 GB a wasm32 heap can address).

- The demo is at
  [seantalts.github.io/stanli](https://seantalts.github.io/stanli/):
  presets, chains sampling simultaneously in one worker each, live trace
  and histogram plots while NUTS runs, split-Rhat and effective sample
  size, and a CSV of the draws.

- An npm package, `@seantalts/stanli`: `compile()` and `sample()` over a worker
  pool sized to the hardware, with an `onLive` callback for streaming
  draws and `preload()` to warm the compiler and runtime before the
  first click. Published on `npm-v*` tags through npm trusted
  publishing.

- The payload is 0.99 MB of runtime plus 0.43 MB of compiler, gzipped. A
  page that ships precompiled MIR never loads the compiler at all.

### A Windows wheel

- `win_amd64` joins the four existing platforms, built under mingw-w64
  (stan-math does not build under MSVC, which is why RStan ships through
  RTools), exporting the C ABI through a `.def` file. It bundles the
  release `stanc.exe` and drives it as a subprocess rather than
  embedding the compiler, which waits on opam's native Windows support.

### What the language covers

- **Truncation and censoring work.** `y ~ normal(mu, sigma) T[0, 10]`
  did not compile before: stanc3 rewrites a `T[,]` into the density
  minus `log_diff_exp` of the bounds' `lcdf`s, and stanli had neither
  piece. Both land here, along with the whole distribution-function
  family: 87 `cdf`/`lcdf`/`lccdf` functions, continuous and count alike,
  every one 0 ULP against CmdStan.

- **34 scalar math functions**: `lgamma`, `log1p`, `Phi`, `inv_Phi`,
  `erf`, `expm1`, `digamma`, the trig and hyperbolic families,
  `floor`/`ceil`/`round`, `inv`/`inv_sqrt`/`inv_square` and the rest,
  on the parameter path and in transformed data and generated
  quantities.

- **18 more distributions**: `chi_square`, `inv_chi_square`,
  `scaled_inv_chi_square`, `frechet`, `gumbel`, `loglogistic`,
  `pareto`, `pareto_type_2`, `rayleigh`, `skew_normal`, `von_mises`,
  `exp_mod_normal`, `beta_proportion`, `skew_double_exponential`,
  `neg_binomial`, `neg_binomial_2_log`, `beta_neg_binomial`, and
  `yule_simon`.

- Coverage is now 46 of Stan's 72 densities, 87 of its 105 distribution
  functions, and 47 of 129 scalar functions, counted against
  `stanc --dump-stan-math-signatures` rather than a table someone typed
  here. [docs/coverage.md](docs/coverage.md) lists what is missing and
  what each gap needs. Every supported function is bitwise identical to
  CmdStan, checked by `harnesses/fn_sweep.py`, which generates a model
  per function from stanc3's own signature list and compares against the
  same reference driver the corpus uses.

- The wheel is bigger for it: 21.3 MB installed, 7.4 MB compressed, up
  from 13.8 MB in 0.2.0. Each distribution is instantiated once per
  activity mask, twice for propto and again for the elementwise form,
  about 630 KB apiece, which is what a precompiled library pays so that
  no model has to be compiled. The long tail of them takes a smaller
  form now, which returned 4.4 MB, and the distributions models actually
  use run exactly as fast as before. The 34 scalar functions cost
  0.03 MB between them.

### Half the library, if you want it

- **`-DSTANLI_LITE_LP=ON`** takes the runtime from 14.9 MB to 7.79 MB
  stripped by dropping stan-math's propto instantiations. A density is
  instantiated once per activity mask, twice over for propto, and again
  for the elementwise variant; dropping the propto half costs only terms
  that are constant in the active arguments, which is why no gradient
  moves. On by default for the browser build, off for the wheels, which
  is what took `stanli.wasm` from 6.2 MB to 3.40 MB raw while *gaining*
  truncation and 76 functions. `stanli_exact_lp()` in C,
  `stanli.exact_lp()` in Python, and `fit.exactLp` in JS report which
  build you have. See [docs/lite-lp.md](docs/lite-lp.md).

### Sampling and generated quantities

- **Initial points are accepted the way CmdStan accepts them**, which
  fixes `lotka_volterra`'s sampling timeout. CmdStan checks a candidate
  by evaluating the log density on doubles and then its gradient; we
  only ever ran the second. That matters for an ODE model, because the
  value path solves the states alone while the gradient path solves the
  coupled state-plus-sensitivity system, and at a solution grazing zero
  the two disagree in sign (measured on the point in question: -1.81e-05
  against +5.33e-06, so log(z) is NaN for one and finite for the other).
  We were accepting starting points CmdStan rejects, and at the corpus
  seed that meant a chain that never left a bad region: lp -1260 against
  a typical set near -14, and 87x the leapfrogs, which is the whole
  timeout.

- **A draw whose generated quantities throw is written as nan and
  sampling continues**, which is what CmdStan does. `stanli_run` used to
  abort and print nothing, so one bad `lognormal_rng` on a marginal ODE
  solution discarded every draw of an otherwise good chain.

- **write_array reached the C ABI**: `stanli_wa_n_columns`,
  `stanli_wa_column_name`, `stanli_wa_seed`, `stanli_wa_row`. Every
  binding gets the columns CmdStan would write, in CmdStan's order,
  including the models whose generated quantities are interpreted per
  draw because the graph cannot express them.

- **`stanli_run` is a self-contained native sampler.** Built with the
  stanc3 embed object it compiles the model in-process: `.stan` and
  `data.json` in, CmdStan-shaped CSV out, with no toolchain and no
  separate compiler binary to find. `cmake --install` now places it,
  `stanli_check`, the shared library, and the headers.

### Faster

- Two kernels stopped doing twice the work. `normal_id_glm_lpdf` built a
  var tape in the forward, threw it away, and built it again in the
  backward to differentiate it; it now differentiates once and stashes
  the partials, as every other native kernel does. `OP_MATVEC`
  accumulated each output element in a single dependency chain, running
  at one multiply-add per cycle; four independent accumulators per sweep
  fill the pipeline. Both are bitwise unchanged.

      diamonds  65,799 -> 35,358 ns/grad   0.48x of CmdStan -> 0.89x
      prophet  103,452 -> 56,912 ns/grad   0.67x -> 1.23x
      blr          877 -> 709 ns/grad      1.97x -> 2.44x

  Their sampling runs followed: diamonds 108 s -> 58 s, prophet 175 s ->
  98 s. Corpus median per-gradient 2.00x -> 2.07x, 93 of 119 models at
  parity or better.

- Both changes came from profiling every sub-parity model with
  `STANLI_PROFILE=1` rather than from guessing, and the same survey says
  where the rest of the tail is: in seven of those models a single
  precompiled kernel is half to nine-tenths of the gradient. Measured
  and rejected along the way: Eigen's gemv (fastest, but reassociates
  and costs 1-2 ULP against stan-math on every model with a matrix),
  cache-blocking the accumulator, swapping the loop nesting (both slower
  than what they replaced), and the same partial-stashing on
  `multi_normal_cholesky_lpdf` (a wash, and it would have cost n^2
  doubles of scratch per op).

### Fixed

- The benchmark table on the PyPI page renders as a table again. The
  marker that stamps generated numbers into the page shared its line
  with the table header, and a line opening `<!--` opens a raw HTML
  block that runs to the `-->`, so PyPI's renderer swallowed the header
  and printed every row as literal pipes. `tools/gen_docs.py --check`
  now renders both READMEs with readme_renderer, the library PyPI itself
  uses, and fails if a table does not come out as one. `twine check`
  never caught this: the page rendered, it just rendered wrong.

### Verification and tooling

- `tools/verify_lite.py` verifies the lite build against the exact one
  (gradients bitwise, lp shift constant across evaluation points), and
  `tools/verify_refs.py --no-lp` replays the corpus for a build whose
  `lp__` is shifted by design.

- `harnesses/fn_sweep.py` takes its function list from stanc3's own
  signature dump, so a function stanli claims and Stan does not offer,
  or the reverse, shows up as a gap rather than as agreement.

## 0.2.0

- Generated quantities and transformed parameters now come out of all
  119 compiling corpus models, up from 93. Where the write_array graph
  cannot express the section (RNG draws, integer draws that then size or
  index things, branches on draw-computed values), a per-draw
  interpreter runs the whole section instead: constrained parameters
  feed in by name, RNG calls draw from a seeded stream through
  stan-math, and `integrate_ode` inside generated quantities works. The
  graph stays the fast path and the sampler is untouched.
- Parameter-dependent branches compile. `if (theta > 0)` and
  `theta > 0 ? a : b` in the model block were compile errors, since an
  op graph cannot pick an arm at evaluation time. The conditional
  region now compiles to a small register program run by one op; its
  backward replays under nested autodiff, evaluating exactly the arm
  CmdStan's generated C++ would.
- The differential corpus oracle runs in CI on every push, on all four
  platforms: recorded CmdStan values for the log density and every
  gradient component replay against each build (measured worst
  deviation 2.6e-12 against a 1e-9 gate). write_array values joined the
  oracle for the 20 models whose generated quantities are
  deterministic. Recording them caught and fixed two interpreter bugs:
  uninitialized reals are NaN as in CmdStan, and batched simplex
  parameters were read transposed.
- Faster: kernel contexts and dispatch resolve once at bind time, the
  executor sweeps unroll 4x, mixture lanes fuse into batched
  elementwise-density and log_mix kernels, and element-store runs fuse
  into vector stores. Median per-gradient 2.00x CmdStan across the
  corpus, 92 of 119 models at parity or better; the ten benchmark
  models span 1.0x-6.1x, and `low_dim_gauss_mix` (0.53x in 0.1.0) is
  now 1.11x. Re-roll's write-fusion renames lazily, fixing a
  compile-time blowup on models that refill one small vector tens of
  thousands of times.
- Initialization draws that produce a non-finite log density are
  rejected and retried, as CmdStan does.
- One MIR interpreter serves transformed data, ODE right-hand sides,
  and interpreted generated quantities with one shared vocabulary, and
  the ODE register machine and the tape-island program are one machine
  with one instruction set.
- Python: `Model.log_prob_grad` raises on a failed evaluation instead
  of returning an uninitialized gradient buffer, and rejects
  wrong-sized points with `ValueError`.
- Tools: per-opcode profiling behind `STANLI_PROFILE=1`, a
  sampler-level differential harness (`tools/sampler_trace.py`), a
  contributor map in `docs/hacking.md`, and doc numbers generated from
  the measured artifacts and checked in CI.

Still true from 0.1.0: `sample()` in Python returns declared parameters
only; transformed parameters and generated quantities reach the CSV of
`stanli_run` but not the Python API yet. No variational inference, no
optimization, no multi-chain threading, no Windows wheel.

## 0.1.0

First public release.

- Stan models compiled and sampled with no C++ toolchain on the machine:
  the real stanc3 is linked into the shared library, models lower to an op
  graph over precompiled stan-math kernels, and the graph doubles as the
  autodiff tape.
- NUTS with diagonal-metric adaptation (`stan::mcmc::adapt_diag_e_nuts`),
  at CmdStan's max tree depth of 10.
- 118 of 120 posteriordb models differentially verified against CmdStan on
  the log density and every gradient component; 45 bitwise identical,
  worst deviation 2.6e-12 relative.
- Per-gradient latency 1.1x to 6.2x faster than CmdStan on nine of the ten
  benchmark models, 0.53x on `low_dim_gauss_mix`. Time to first draw
  roughly 20x faster, since there is no compile step.
- Graph passes: loop re-rolling turns unrolled per-observation loops back
  into vectorized ops (`radon_pooled` goes from 27,670 ops to 8),
  destructive functional updates, store-to-load forwarding, dead-write
  sweeping, and constant folding of the ops no parameter reaches.
  `STANLI_NO_REROLL=1` disables re-rolling.
- ODE right-hand sides compile to a flat register machine instead of being
  walked as a tree, and one solve produces both values and sensitivities:
  29x to 39x on the models that integrate. `STANLI_DEBUG_ODE=1` reports
  when a right-hand side falls back to the interpreter.
- Transformed parameters and generated quantities are computed by a
  second forward-only graph and written by the command line tool for 93 of
  the 119 compiling corpus models.
- Wheels for macOS arm64 and x86_64, Linux x86_64 and aarch64
  (manylinux_2_28). 13.8 MB installed.

Known gaps in the Python API: `sample()` returns declared parameters only,
so transformed parameters and generated quantities are not surfaced yet.
No variational inference, no optimization, no multi-chain threading, no
convergence diagnostics, no Windows wheel.
