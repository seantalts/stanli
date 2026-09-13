# Changelog

## 0.13.0

### The upstream loop vectorizer's new shapes lower without regressions

The compiler runs stanc3's `vectorize_loops` pass, and the 0.12.0 compiler
pin widened what that pass rewrites: gathers, elementwise arithmetic,
independent statements in one loop body, and assignment loops. Three of the
new shapes cost more than they saved. A whole-row write into a matrix,
`p[j, :] = v`, lowered one element at a time; it now takes the same strided
store as `p[j] = v`. A statically empty range assignment, which the pass
makes from a loop such as `for (i in 1:0)`, lowered its whole right-hand
side; it now lowers nothing. A vector-valued `+`, `-`, `*` or `/` between
two scalar runs split an island in two, and the half left over was priced
out and interpreted: iohmm_reg's log density went from 27 ops to 10522 and
each gradient took 29% longer. Islands now compile elementwise vector
arithmetic up to 64 elements wide, so the run stays whole and iohmm_reg is
back at parity. Across the 26 corpus models the pass rewrites, gradient
time is now at or below the pass-off figure everywhere.

The vectorization harness gates on op counts for every model whose MIR the
pass changes: the lowered log density may not grow, and the final graph may
grow by at most 10%. Its gradient benchmark set is now the models the pass
changes. `harnesses/corpus_bench.py` measures the stanli columns through the
shipped compile pipeline, and can build the CmdStan side with a chosen stanc
binary and flags; a manifest next to the TSV records which.

### Passes generalized past the shapes the corpus showed

Each of the fixes above was one spelling of a narrower assumption in a
pass. The assumptions are gone.

The re-roll pass packs lanes that walk the rows of a container. A lane was
one element wide, so every per-row loop, `y[i] ~ multi_normal(mu[i], Sigma)`
over an array of vectors or a likelihood per subject, ran one vector op per
row after unrolling. A lane may now be a whole row: row slices of an
invariant base collapse to the base itself, per-row constants and outcomes
pack in the base's storage order, and a run of row stores covering every
row makes the fused value the container. dogs now runs one density over
its 750 trials and is 19% faster with the pass on than off; dogs_log 25%;
mother's array-of-vectors prior drops from 114 to 71 ops.

Every static indexed assignment lowers through the same selector map the
reads use: one contiguous, strided or scattered store in any dimension,
bounds checked at compile time. The list of hand-written spellings is gone.
Two silent out-of-range writes (`z[4] = v` on an `array[3] vector`,
`m[5, 1]` on a `matrix[3, 4]`) are compile errors now, and a store through a
repeated index list such as `m[{2, 2}, 1]` no longer credits every
right-hand element in the gradient. A selector that picks nothing lowers
nothing.

Islands compile every elementwise op at any width as one range instruction
with broadcast, so no vector op ends a run, and the carver prices the joined
run, the run split at vector ops, and leaving the ops alone, and emits the
cheapest. The estimate now charges reductions by the width they touch and
does not charge absorbed constants. This matters because joining is not
always a win: the per-element island version of a wide vector op can be
slower than the graph kernel, and op counts do not show it.

`stanli_check` compiles through the embedded pipeline, the one `stanli_run`
and the packages use, so every lit case and corpus check now verifies the
MIR that ships. A build without the embedded compiler runs `stanli-compile`
as a subprocess; `--stanc PATH` remains as an explicit opt-in for A/B work.

Generated quantities and transformed parameters are now written by each
chain as it samples. The work happens on the chain's own thread as each
draw is stored, so it runs in parallel across chains and inside the
sampling progress the user already sees. R and Python no longer constrain
the stored draws after the run finishes, a phase that showed no progress
and could take a long time on a model with tens of thousands of output
columns.

The runtime can now be built with `STANLI_NO_STDIO` defined, for R
packaging: the object library then contains no stdout, stderr, abort, or
assert-failure symbols. Debug traces that used to write directly to stderr
(island carving, structured-loop diagnostics, the preparation profiler) now
go through `emit_diagnostic`, a sink-backed channel parallel to `print()`'s
existing `emit_message`. The preparation profiler no longer aborts if a
model exceeds its previous fixed row count; it grows instead.

The island interpreter's two entry points are aligned to a cache line.
Growing the runtime moved them to 60 bytes into a line, and the same island
program ran 5.5% slower on hmm_gaussian and hmm_drive_0 with islands on and
identically with them off; aligned, both are back at parity with the
previous release and the small brms islands lost most of their drift.

A density merged across loop lanes may sit up to 30 ULP from CmdStan, since
one call sums what CmdStan sums per iteration; dogs measures 31 and 32 at
two recorded points. Against a 60-digit reference both engines are off by
about as much there, CmdStan by its one-term-at-a-time sum and stanli by
Eigen's packet reduction, so the gap is a difference in rounding rather
than an error on one side, and `tools/corpus.py` records it as such.

### RNG functions in transformed data

Transformed data can now call `_rng` functions. Draws come from the
construction seed the way CmdStan's generated constructor seeds them, so a
matched seed reproduces CmdStan's transformed data bit for bit. Python
`Model(seed=1)`, R `stanli_model(seed = 1)`, `stanli_run --seed` and the C
API's `stanli_model_new_seeded` set that seed; the unseeded constructors use
1, and BridgeStan passes its construction seed through. One seed governs a
run, as in CmdStan: Python `sample` and `optimize` and R `sample_model` and
`optimize_model` rebuild the model under their own seed when transformed
data drew and the seed differs. Models without transformed-data draws are
never rebuilt. An `_rng` call inside a user-defined `_rng` function failed
with "unknown variable" in the interpreted write_array; it works now.

### Executors share model data

Model data is shared across executor copies. Parameters, written outputs,
adjoints and scratch stay private, and taking a writable pointer detaches
that executor's copy. Eight MNIST executors peak at 4.8 GB of memory where
they peaked at 10.3 GB. Parsed JSON arrays move straight into the data map,
and the exponentiated-quadratic GP kernel with fixed coordinates and
Cholesky decompositions reuse their saved forward results in the backward
pass in place of a nested autodiff tape, so gp_regr's gradient takes 2.6 µs
where it took 6.7.

### Every stanc signature replays against CmdStan

The generated builtin and density signature models used to prove only that
each overload compiled and ran. They now replay against recorded CmdStan
references at the three corpus points: log density, the full gradient, and
every output value, from transformed data, the model graph, a
runtime-control region and generated quantities alike. Each overload is
called once with every real argument a parameter and, when it has two or
more, once per argument with only that one a parameter, so the mixed
data/parameter instantiations CmdStan's generated code selects are
exercised too. Every case reads its own parameter and writes its own
output, so each gradient element and output column is compared for one
overload at the corpus replay's 1e-9 gate and a failure names the overload.
`tools/check_signature_models.py --record` refreshes the references from a
CmdStan checkout; a partition whose source moved refuses to replay until it
is re-recorded. The sweep found the three fixes below.

### Fixes

A threaded run now polls its interrupt callback once before starting the
chains, so a stop that is already pending ends the run before the first
transition instead of racing the worker threads.

`beta_neg_binomial_cdf`, `_lcdf` and `_lccdf` took about a second per call
on aarch64 Linux. Boost's default policy evaluates double special
functions in long double, which there is software binary128, and its
epsilon set the length of the hypergeometric series. The build now turns
that promotion off on non-x86 targets, so those platforms evaluate in
double instead. x86 keeps the promotion, since there long double is the
hardware 80-bit type and matches CmdStan's reference results.

`bernoulli_logit_glm`, `poisson_log_glm` and `neg_binomial_2_log_glm`
dropped the gradient of a parameter design matrix: the log density was
right and `x`'s adjoint came back zero.

`lkj_corr_lpdf` and `lkj_corr_cholesky_lpdf` ignored a parameter `eta`,
returning no gradient for it and, with `propto`, the wrong value, because
the kernel's differentiable set excluded the argument the model activated.
A density plan whose active arguments fall outside its kernel's
differentiable set is now refused at lowering rather than silently treated
as data.

`fmax` and `fmin` inside a runtime-control region followed the `var, var`
overload whatever the operands' types. stan-math breaks a tie towards the
autodiff argument, the second when both are, and a data side never carries
an adjoint; the register program now compiles each call to the instantiation
its operands select, so ties and NaN operands route the gradient the way
CmdStan does.

A shape query on a local declared earlier in the same block, `int n =
rows(m)` with `m` a local matrix, sent a model's transformed parameters
and generated quantities to the MIR interpreter. `--O1` inlining declares
every user-defined function's locals this way, so any generated-quantities
call of a function that asks the extent of its own local paid the
interpreter's cost. The write_array scan now answers such a query from the
local's declared type, as the lowering after it does.

The browser compiler bundles load on Safari 17 and iOS 17 again.
js_of_ocaml 6.4.1 puts a line break between `static` and the class field
it modifies, and JavaScriptCore 17 reads that as a field named `static`,
so the compiler's 64-bit integer constants were undefined and the first
compile threw. The build now joins those lines and fails if any remain.
The reporter also fixed the printer upstream in ocsigen/js_of_ocaml#2421
(#342).

The runtime no longer keeps a `thread_local` object with a destructor.
Island registers, native adjoints and solver workspaces live in executor
scratch, and the executor pool's autodiff tape is leased from a free list
the pool owns and returned empty when a thread's last lease ends. Under
MinGW's emulated thread-local storage those destructors ran after the DLL
that owned them was unloaded, which crashed R worker processes on Windows.
A worker thread with no autodiff stack of its own, the case when BridgeStan
is driven from the caller's threads, now takes a gradient about as fast as
the main thread.

The adjoint ODE solver handed stan-math an uninitialized quadrature vector
on the first backward step, and stan-math accumulates into it without
assigning it first, so a backward solve could fail depending on what the
allocator returned. The pinned stan-math is patched at checkout to zero the
vector.

## 0.12.0

### Sampling can be interrupted

Ctrl-C in R, or the stop button in RStudio, now stops every chain after its
current transition and raises the usual interrupt, on every platform. Before,
the interrupt could only land between chains, and on Windows it could take
the session down. The C ABI gains `stanli_sample_multi_interruptible`, the
progress sampler plus a poll callback asked on the calling thread about every
100 ms; the R package uses it when the runtime provides it. The Python
package does the same: Ctrl-C during `sample()` stops the chains and raises
`KeyboardInterrupt`. (#327)

### R names indexed values with brackets

`model$columns`, and with it every draw, summary and diagnostic name, now
spells an indexed value `theta[1,2]`, the form the posterior package and the
rest of the R Stan tooling read, instead of the CSV header's `theta.1.2`.
(#328)

### Python reads variables by shape

The Python package spells indexed values the same way: `Model.constrained_names`,
`Fit.names`, summaries and `optimize()` results all use bracket names now.
`Fit` and `OptimizeResult` also index by variable name: `fit["theta"]`
returns an array with the declared dims rather than one flat column, and
`to_arviz()` passes those dims through to the posterior group.
Dot names like `theta.1` are still accepted wherever a name is looked up.

### A model that leaves the compiled path says so

When the graph could not lower a model's transformed parameters and generated
quantities, or the register program could not compile an ODE, DAE, algebraic
or quadrature callback, the MIR interpreter took over that part, around a
hundred times slower per evaluation, and nothing said so. The model now
carries a warning that quotes the lowering's reason and asks for a bug report:
the R package raises it with `warning()`, the Python package as a
`RuntimeWarning`, `stanli_run` prints it to stderr, and `stanli_warnings`
returns it from the C ABI. A model whose interpreted section fails at every
probe point, which used to drop its columns silently, gets the same text.
Setting `STANLI_NO_INTERPRETER=1` turns the warning into a compile error.

A runtime-control region may now use 2^20 registers rather than 2^16. The
generated quantities of `hmm_gaussian` and `iohmm_reg`, the two posteriordb
models that reached the interpreter, lower to the graph within that and match
the CmdStan references; their write_array row costs the same either way.

### Fixes

A retained loop no longer answers a reduction from a slice's storage capacity.
A slice whose upper bound is a loop-carried integer, `t[1:k]` with `k` advanced
by a `while`, keeps its declaration's capacity as storage and the read leaves
the unselected tail at zero. Only `sum` consulted the live length, so
`log_sum_exp` reduced over the capacity and the zeros with it, `max` returned
zero, `num_elements` returned the capacity, and a density over the slice
integrated the tail. The loop form of such a model answered 1.863 where CmdStan
answers -0.819. Nothing had to be turned on to reach this: a `while` whose guard
is data lowers as a retained loop by default. The live length is now an operand
of the ops that consume the slice, and the loop rewrites their operand lengths
from it before each call, forward and backward, so the reductions, the
elementwise ops, the dot products and the densities all stop where the values
do and the backward stops there rather than scattering adjoints into the tail.
An operand shape this cannot describe now refuses at compile time instead of
answering from the capacity, and a full-width operand beside a runtime-length
one is one of those shapes: `sum(t[1:k] .* u)` used to narrow `u` to `k`
silently, where the unrolled path and CmdStan both call it a size mismatch.
`rows` and `cols` of a runtime submatrix and `size` of a slice of an array of
vectors now answer from the axis the query names instead of declining, and a
gather through a runtime-length selector, `y[idx[1:k]]`, no longer takes the
whole loop down with it.

`pow` keeps its base gradient at a base of exactly zero wherever stan-math
does, and the exponent's static type is what decides, as in stan-math.
stan-math's reverse-mode `pow` sends a non-var exponent of 1, -1, -2 or -0.5
to the base itself, `inv`, `inv_square` or `inv_sqrt` before it reaches its
zero-base guard, so those four carry a partial where the guard carries none,
and stanli returned zero for all of them. The type stanc3 emits is what
selects the overload, so a model-block local holding a data value is a `var`
and stays on the guard, while a transformed-data real or an integer
expression is a `double` or an `int` and takes the redirect. An Eigen base
against a scalar exponent takes the matrix redirects, whose `inv_square` is
NaN rather than infinite at zero; an Eigen base against an Eigen exponent has
no redirect at all; a loop of scalar calls that reroll widened keeps the
scalar answer each of its lanes had. brms models built with `ar(cov = TRUE)`
reach this through `cholesky_cor_ar1`'s `pow(ar, i - 1)`, whose
autocorrelation gradient was zero at every point where `ar` is zero.

`choose` is available wherever an integer is evaluated when the model
compiles: a transformed data int, a declaration extent, and an index whose
argument is an unrolled loop variable. brms writes `cor_1[choose(k - 1, 2) +
j]` to flatten the upper triangle of a group-level correlation matrix, and a
model that used it lost its whole generated quantities section to the
per-draw interpreter, which then refused `lkj_corr_cholesky_lpdf` and left
`stanli_run` writing no CSV at all. A model with a `logistic_normal` response
sizes a transformed data array the same way and did not compile.

The corpus replay fails a model that produces no write_array row. The
recorder drops the reference for a row the two engines disagreed about, so a
model whose write_array failed outright recorded none and was then replayed
as if it had no section to check. Every model has a row, so presence is
demanded whether or not the reference carries one. Both models that lost
their generated quantities to `choose` passed the gate this way.

The write_array graph emits a column whose variable `--O1` substituted away.
A transformed parameter with a constant value, such as the `real disc = 1`
brms writes in every ordinal model, reaches the write as the literal rather
than as a name, and the graph gave up the whole section for it. The column
takes its name from the declared emission order, the same rule the per-draw
interpreter has used. Ten of the shipped brms models, every ordinal family
with and without `cs()` among them, stop falling back.

The per-draw interpreter carries `lkj_corr_cholesky_lpdf` and `lkj_corr_lpdf`,
which the write_array graph already had. It is the fallback for any section
the graph cannot express, so a model that writes an LKJ density in
transformed parameters or generated quantities no longer depends on the
graph covering everything else in the section.

A runtime-control region can hold the integer-outcome densities. A region is
what a model compiles to where its control flow depends on a parameter, and
its density vocabulary was the shared scalar list, which holds the continuous
densities only, so `target += poisson_log_lpmf(y | eta)` under an `if` or a
`while` on a parameter was a compile error for a line the flat path handles
everywhere else. `poisson_lpmf`, `poisson_log_lpmf`, `bernoulli_lpmf`,
`bernoulli_logit_lpmf`, `binomial_lpmf`, `binomial_logit_lpmf`,
`neg_binomial_2_lpmf` and `neg_binomial_2_log_lpmf` now reach the same graph
kernel the flat path calls, so the region's value and gradient are that
kernel's to the bit. The outcome, and the number of trials for the binomials,
must be an integer the region knows when it compiles. The GLM forms are
unchanged: their data matrix is a different argument shape.

`log2()`, `log10()` and `sqrt2()` evaluate. Stan's nullary constants are one
family and six of the nine were recognized, so these three failed to compile
on every path with `unsupported function log2`. brms writes `-23 * log2()` as
the convergence tolerance of the COM-Poisson normalizing constant. The values
are stan-math's own, and the recorded CmdStan reference for the function
coverage model holds them to the bit.

A `while` whose body declares a local sized from the loop's own state now
unrolls where the guard is data, instead of compiling as a loop whose counter
sits in a register. A declared extent has to be a compile-time integer in
both loop forms, so `array[nobs[i]] int iobs` inside `while (i <= I)` used to
fail with a runtime-control region error naming the extent. brms writes
exactly that in `normal_time_hom_flex_lpdf`, the log density it emits for
`unstr()` autocorrelation, so those models now compile. A `while` that does
not size a local this way keeps its loop form as before.

The scan that decides whether a loop needs a runtime-control region mirrors
block-local integers in statement order, the way the write_array scan beside
it already did, so an early return guarded by a local count no longer forces
the enclosing loop into a region.

The GLM densities take a per-row vector intercept. `bernoulli_logit_glm_lpmf`,
`poisson_log_glm_lpmf` and `neg_binomial_2_log_glm_lpmf` refused one, and
`binomial_logit_glm_lpmf` read only its first element while its backward wrote
past the partials the forward stored. brms emits a vector intercept for every
model with a group-level term, so hierarchical count and binary models
generated by brms now run.

An initialization error reports what the model threw. A draw that raises is
still a rejected draw, but the first message is kept and appended to the
error, so an unsupported shape no longer surfaces only as "no draw in 100
attempts had finite log density and gradient".

A shape query (`num_elements`, `size`, `rows`, `cols`) is now answered for
elementwise arithmetic, `transpose`, and a matrix row selected by a loop
variable. Before it was answered only for a named value and for a matrix
subview at a position known when the model compiled. brms writes its
category-specific ordinal models (`sratio`, `cratio` and `acat` with `cs()`)
as a log density that sizes its locals with `num_elements(thres)` and passes
`Intercept - transpose(mucs[n])` for `thres`, so those sizes stayed unknown
and the models failed to compile with a runtime-control region error.

A shape query is now also answered for a gathered submatrix, such as
`x[idx, idx]` with `idx` a data array. Before, only a subview at a position
known when the model compiled answered, so a user-defined function that
takes `rows()` of its gathered-matrix argument bailed the whole
runtime-control region as an unknown compile-time integer.

Real data may be infinite or not-a-number, as it may in CmdStan. The JSON
reader accepts `Infinity`, `-Infinity`, `Inf`, `-Inf` and `NaN` both bare and
quoted, the two spellings CmdStan's reader takes. brms writes these routinely:
a one-sided truncation passes `-Inf` as the unused bound, and `mi()` fills the
missing rows of the response with `Inf`. Data declared `int` still has to
arrive integer-valued. A declared bound follows Stan, so an infinite value
satisfies `real<lower=0>` and a not-a-number value does not.

The R data writer emits those tokens rather than refusing the value, and `NA`
in a double becomes `NaN`; only integer and logical data still refuse a missing
value. The browser API used to hand the runtime `null` for a non-finite number,
which is what `JSON.stringify` writes, and now writes the tokens too. A brms
`make_standata` list reaches a model unchanged.

`gp_matern32_cov`, `gp_matern52_cov` and `gp_exponential_cov` compile and
evaluate wherever `gp_exp_quad_cov` already did, with gradients for the
coordinates, the marginal scale and the length scale. brms models written
with `gp(x, cov = "matern32")` and the other kernels brms offers now run
(#320).

The multi_normal densities accept an array of locations, the shape brms
gives a model built with `set_rescor(TRUE)`. An array of locations pairs
elementwise with an array of random variables of the same length, or
broadcasts against a single random variable, matching stan-math.

64 models generated by brms 2.23.0, checked in under `tests/brms/` exactly
as `make_stancode` and `make_standata` wrote them, carry recorded CmdStan
references and are replayed by `tools/verify_refs.py` on
every push alongside posteriordb and the stanc3 language fixtures.
`tools/gen_brms_models.R` regenerates them. All 64 match CmdStan at the
three evaluation points. A model stanli comes to refuse is listed in
`KNOWN_GAPS` with what stops it, and a listed model that starts passing
fails the run until its entry is deleted.

A second sweep adds 60 more, for 124 in all: the remaining response
families, the multimembership and by-group grouping terms, the spatial
and autocorrelation structures, the addition terms, custom families with
their own `stanvar` functions, and the approximate and grouped Gaussian
processes. Recording them found two things. `pow(x, n)` reported a zero
derivative at `x == 0`, which is the whole `ar` gradient of a model
written with `ar(cov = TRUE)` at two of its three points. And a
`write_array` that fails did not fail the corpus gate, so five models
whose gradients were right had been producing an empty CSV from
`stanli_run` unnoticed. Both are fixed above, and one of the new models
is refused today: `s2_com_poisson` is listed in `KNOWN_GAPS`.

The interpreter accepts whole-value index nodes and column writes into
integer arrays produced by loop vectorization; before this stanli_run failed
on eight corpus models that stanli_check passed.

### Faster

NUTS calls the executor's gradient directly. The model adapter used to
answer every leapfrog step through a var tape, and that round trip was
12 to 30 percent of sampling time on models whose gradient is cheap
relative to their parameter count. `bym2_offset_only` (1000 warmup, 1000
draws) went from 15.2 s to 12.6 s over the same 189728 gradient
evaluations. `tools/bench_grad.cpp` now times the sampler's real gradient
call, `stan::model::gradient`, so the stanli gradient column is comparable
with the sampler and slightly different in kind from earlier tables.

## 0.11.1

### Fixes

The generated reverse pass no longer reads out of bounds when an island
contains a zero-length range copy.

A dispatch buffer for calls into higher-order kernel backwards (ODE adjoint
solves, quadrature, DAE, islands) was shared across recursive invocations.
When an island's own generated backward itself called another such kernel,
the inner call overwrote the arguments the outer call still needed after it
returned, producing a wrong gradient for models that nest one of these
inside another. The buffer is now allocated per call.

### Retained loops store one version per write

The structured loop executor (`STANLI_STRUCTURED_LOOPS`) decides every body
kernel's storage when the model compiles: data-only intermediates read once
reuse a fixed cell, values with a reverse pass are appended to a growing tape,
and `x[i] = v` mutates its container in place with an undo log, so gradient
memory is proportional to the work done rather than to container size times
trip count. `while` loops no longer need a compile-time iteration bound and
`for` loops accept bounds without a known range. `STANLI_STRUCTURED_HISTORY_BYTES`
and `STANLI_STRUCTURED_MEMORY_PROFILE` are gone.

A retained loop records its first evaluation: work that depends only on data
is replayed from that record afterwards, data-only branch and loop decisions
come from a trace, and iterations of a data-controlled loop in which nothing
observable happened are skipped. On ctsem one gradient at 400 rows fell from
4.4 s to 0.32 s with identical results. Straight-line runs of scalar body
kernels run as one register program with a generated reverse pass, so a
scalar recurrence costs one tape record per iteration instead of one per
kernel (`STANLI_NO_STRUCTURED_SEGMENTS=1` keeps every kernel its own node).

The recording evaluation hands back the storage of a data-only subtree whose
values nothing reads once it exits, so the first gradient of a retained loop
no longer costs several times the steady-state tape. ctsem at 400 rows peaks
at 0.95 GB instead of 1.24 GB.

A retained loop's dynamic index kernels resolve their selector once per call
instead of once per selected element, and their validation no longer carries
message building in the path that takes no error. ctsem is 14% faster per
gradient and radon_county's retained loop 20%.

By default a loop is retained when it is an outermost `while`, or an
outermost `for` of at least 32 iterations whose body contains a `while` or a
branch chosen by a parameter; every other loop unrolls as before.
`STANLI_STRUCTURED_LOOPS=1` now changes only which loops are retained.

## 0.11.0

### Call Stan functions from Python

`stanli.Function` exposes pure, value-returning Stan UDFs through a separate
Python/NumPy adapter. It accepts source files, source strings, or cached MIR;
calls use named Python arguments and typed numeric buffers, preserving integer
identity and logical array dimensions without JSON serialization. Results are
owned Python scalars or NumPy arrays. `tools/bench_python_function.py` compares
steady-state call latency with plain Python and vectorized NumPy.

Repeated calls now pack exact Python `float`/`int` scalars directly, bypassing
NumPy conversion. Native handles retain immutable function lookup tables;
overload selection and validation still run per call. Argument storage and
interpreter state remain call-local, including for concurrent/reentrant calls.

### Large data-dependent loops compile as native graph regions

Stan programs whose loop trip count is known only from data, with
parameter-dependent control flow in the body (the shape of ctsem's
Kalman-recursion `while` loop, for example), previously had that loop
unrolled fully into the compiled graph, which becomes expensive to prepare
and hold in memory once the trip count and nested state grow large. Such loops can now compile to a
native graph region that walks the body at runtime instead of unrolling it.
The path is a conservative addition: unsupported bodies fall back to the
exact prior unrolling behavior, and existing models continue to compile and
match bitwise (#248).

Follow-up work reduced the memory this runtime representation needs by
reusing loop-invariant results across iterations and storing per-iteration
history compactly, so larger instances of these models compile within
practical memory instead of exhausting it.

### Parameter-dependent control flow supports more of Stan

The register-program path, used to run `while` loops and other control flow
that depends on parameters, now supports `print` statements without
duplicating their output during autodiff replay, and `reject()` with a
runtime-constructed message, consistent across the C API and BridgeStan. It
also lowers Jacobian transforms for all fifteen constrained-parameter types.

Recursive and data-dependent user-defined functions, mixed integer and real
arguments, and additional regular functions such as `sin`, `atan2`,
`tgamma`, Bessel and factorial functions, `ldexp`, `lmgamma`, and
`binary_log_loss` now work inside this control flow and inside ODE
right-hand sides, with reverse-mode gradients computed alongside the rest of
the graph. Reductions such as `prod`, `min`, and `max` also preserve Stan's
argument grouping across range, gather, indexed-container, and
user-defined-function-result arguments.

### Sampling can start from Pathfinder draws

Python, R, and the browser can now start NUTS sampling from unconstrained
draws produced by a single Pathfinder path instead of explicit initial
values, with validated tuning options and reproducible seeding across
chains. Pathfinder initialization and explicit initial values are mutually
exclusive, existing explicit-initialization behavior is unchanged, and
Pathfinder's own PSIS resampling is skipped since only the initial draws are
needed (#303).

### Initial values can be given on the constrained scale

stanli had the forward parameter transforms but not their inverses, so every
initialization surface accepted only unconstrained values. Python's
`Model.unconstrain()` and R's `unconstrain()` now convert a constrained
parameter draw to the unconstrained vector a sampler expects, including
bounds that depend on an earlier parameter such as `vector<lower=alpha>`.
The same conversion is available through the C API and through BridgeStan's
`bs_param_unconstrain`, `bs_param_unconstrain_json`, and
`bs_param_initialize`.

### Individual Stan functions can be called from C++

A new `stanli::Function` API evaluates pure, value-returning Stan
user-defined functions directly, without compiling or running a full model,
similar to the R interfaces' popular `expose_stan_functions`. It accepts a
model source file, source string, or cached MIR, takes arguments by name
through a `DataMap`, and returns a shape-preserving result, with overload
resolution and argument validation handled the same way a model's own
function calls are.

### reduce_sum compiles and runs

Stan models that call `reduce_sum` now compile and run. Slices execute
serially; parallel or threaded execution is not yet implemented.

### More built-in functions, RNGs, and densities lower

`reverse`, `block`, `to_matrix`, the `linspaced_*` and
`zeros_`/`ones_`-prefixed constant-fill families, `identity_matrix`,
`csr_extract_v`/`csr_extract_u`, and a generalized `rep_array` (scalar or
container elements, over one to three replication axes) now compile and
evaluate.

`gp_exp_quad_cov` and `normal_id_glm_lpdf` accept a parameter (non-data)
matrix argument, `is_inf` is supported alongside `is_nan`, `not_a_number`
and `negative_infinity` join the existing nullary math constants,
`profile(...)` is treated as a transparent wrapper instead of failing, and
generated-quantities density accumulation covers
`neg_binomial_2_lpmf`/`neg_binomial_2_log_lpmf` and the non-Cholesky
`multi_normal_lpdf`.

`gumbel_rng`, `dirichlet_rng`, and `beta_binomial_rng` are supported in
generated quantities and, along with `exponential_rng`, inside
parameter-dependent runtime-control regions such as a `while` loop, so a
model no longer drops an entire section to the interpreter just to draw
from one of these inside such a loop.

`multiply_lower_tri_self_transpose` returned an incorrect result in the
compiled graph for any matrix with a non-zero upper triangle; it now calls
Stan Math's own implementation in both the graph and the register-program
backend.

### The browser shows sampling diagnostics and generated quantities

Completed NUTS runs in the browser now show the same diagnostic report the
Python and R samplers print: divergences, maximum-treedepth saturation,
E-BFMI, rank-normalized R-hat, and bulk/tail effective sample size across
chains, computed in a background worker for both single-run and comparison
views. WALNUTS runs mark these diagnostics as unavailable rather than
showing a stale NUTS-shaped report, and Pathfinder keeps its existing k-hat
display. A run too short to compute a diagnostic reports it as incomplete
rather than implying a clean run, and a run whose draws are already
available keeps them if diagnostic reporting itself fails (#292).

Generated quantities are shown in a collapsed table section, and the
selected parameter's plot no longer disappears while navigating between
parameters with the arrow keys.

### Generated-quantity export works on Windows again

The Windows build was missing the exported `stanli_wa_n_generated_start`
symbol needed to locate where generated quantities begin in a run's output,
which broke `test_capi.exe` and any client linked against the Windows DLL,
including BridgeStan clients. The export is restored.

## 0.10.0

### ctsem's structured-control vocabulary lowers

`crossprod`, `tcrossprod`, scalar/vector `add_diag`, `matrix_exp`,
`mdivide_left`, `mdivide_right_spd`, and `quad_form_sym` now have the native
and structured-region semantics needed by ctsem, with Stan Math value and
gradient references and generated-quantities coverage. Parameter-sensitive
`while` statements compile to runtime jumps with loop-carried integer state
and autodiff replay rather than a fixed compile-time unroll.

Data-only integer-array UDF results now retain their value-dependent extents,
including empty results and multiple matches, and work in log density and
write-array lowering. Multidimensional integer arrays preserve their logical
shape when moving between interpreter and graph layouts, and `append_array`,
`sum`, static selections, and conditional UDF returns consume those values
inside structured regions.

Indexed assignment support now includes `IndexMulti` scatter writes in the
MIR interpreter and write-array path, including multidimensional selections
and repeated-index last-write-wins behavior.

The MIR-interpreter write-array fallback now evaluates
`multi_normal_cholesky_lpdf`, including `array[N] vector[K]` observations with
first-index-fast storage.

### Loop-invariant target terms do not unroll

A `for` loop whose body only adds iterator-independent terms to `target` now
lowers once and multiplies those terms by the data-computed trip count. The
ten-million-iteration and 4,000-by-1,440 nested reproducers from #248 both
compile to two ops with baseline memory use; loops with assignments to outer
variables, print/reject, or other observable effects retain the ordinary
per-iteration path.

### Compiled RK sensitivities avoid nested autodiff

Eligible `ode_rk45` and `ode_ckrk` calls now integrate values and coupled
sensitivities directly from the compiled RHS value and Jacobian program. The
existing Stan Math path remains the exact fallback for unsupported RHS
instructions and can be selected with `STANLI_NO_ODE_DIRECT_RK=1` for
differential testing. This removes nested reverse-mode autodiff from the hot
callback while preserving the pinned Stan Math tableau, controller, error
norm, output layout, validation, and exception behavior.

### Infinite declaration bounds are identities

Parameter declarations whose lower bound is negative infinity or whose upper
bound is positive infinity now pass those elements through unchanged and add no
Jacobian term, matching Stan Math. This also works elementwise when a vector of
bounds mixes finite and infinite entries (#250).

### Full-span reads lower

`m[:, :]` and `v[:]` compile instead of failing with `unsupported index
expression`. Upstream O1 folds a full-span read's `All` indices away and
leaves an index-less `Indexed` node, which reached no lowering path.

### More matrix selections and compile-time locals lower

Two-axis matrix selections now accept any combination of `:`, single indices,
ranges, and integer arrays, so `M[1:2, 2:3]`, `M[idx, 1]`, and `M[1, idx]`
lower with the shapes CmdStan gives them. Lowering also folds `ceil()` and
scalar comparisons of data-only reals, keeps compile-time values across
self-referential assignments such as `position += step`, and retains the
bindings of containers filled through indexed assignment, which together let
data-only `while` loops and gathers by locally built index arrays compile
(#247).

## 0.9.6

### Sampling reports progress and problems

The Python and R samplers now print CmdStan-style per-chain iteration updates,
warmup/sampling timings, and warnings for divergent transitions or maximum
treedepth saturation. A `refresh` argument controls the update interval and
`refresh = 0` keeps sampling quiet. Progress events are marshalled back to the
calling thread, so parallel R and Python console output is safe, and reporting
does not change draws, sampler statistics, or RNG streams (#230).

### Complete indexed assignments lower everywhere

Assignments such as `x[:] = rhs` now preserve the destination's declared
shape and integer representation in transformed data, graph lowering,
parameter-dependent register programs, ODE functions, and generated
quantities. This also prepares the runtime for assignment loops rewritten by
the upstream `vectorize_loops` pass.

## 0.9.5

### Upstream MIR loop vectorization is enabled

Every portable compiler now applies upstream O1 plus the separately selected
`vectorize_loops` pass in the shared OCaml pipeline. The pass-off pipeline
remains an explicit test oracle, and the existing C++ loop re-rolling pass
remains enabled because the complete measurement still finds supported models
that benefit from it.

## 0.9.4

### R and webR use the shared portable compiler

The compiler bundled in the R package is now the same exact-source
js_of_ocaml artifact used by the browser and npm package. V8 and webR select
its `stanli_compile()` export and send compact Portable MIR v2 directly to the
runtime; a selected producer error remains final rather than being retried
through the compatibility `stanc()` export.

The artifact is byte-compared with a fresh shared-compiler build and records
the stanc3 revision, OCaml target, Dune and js_of_ocaml versions, every producer
input hash, and its own SHA-256. The release matrix runs this package's compact
compiler against both its current runtime and the released v0.9.3 dual-reader
runtime, then runs the v0.9.3 package's legacy compiler against the current
runtime. Native subprocess, V8, and webR source-compilation paths remain
separate exercised gates.

## 0.9.3

### Portable MIR is compact and shared

The native, browser, npm, and Windows compilers now run one stanli-owned OCaml
pipeline over upstream typed MIR (#211, #213). Native OCaml, js_of_ocaml, and
the Windows executable emit byte-identical canonical output for ordinary
models, nested user functions, loop control, checked integer folding, exact
floating-point payloads, Unicode, and includes. Browser and npm packages prefer
the shared compiler while keeping pristine stancjs as a one-release rollback;
Windows packages similarly prefer `stanli-compile.exe` and retain `stanc.exe`.
A selected compiler failure is reported directly rather than hidden by retrying
another producer.

The first portable JSON envelope never shipped and has been replaced by compact
Portable MIR v2 (#218). The reader builds the C++ MIR directly, enforces
canonical base64 and bounded allocations, and retains the legacy S-expression
reader. Across the 153-program fixture census, v2 uses 823,104 bytes versus
4,652,169 for legacy MIR. Eight Schools decodes in a 0.074 ms fresh-process
median versus 0.293 ms for legacy and occupies 6,932 bytes versus 33,320.

### The R and webR bridge is ready for the shared compiler

The real V8 and webR helpers recognize `stanli_compile()` by export presence,
preserve exact UTF-8 bytes, and keep a selected producer's error final (#225).
This release intentionally retains the legacy JavaScript compiler inside the R
package while its runtime gains the compact-v2 reader. It is therefore the
compatibility anchor for the next release: the next package's v2 compiler can
run with this runtime, and this package's legacy compiler can run with the next
runtime.

### Selective upstream MIR passes can be measured independently

The shared OCaml pipeline now has an explicit pass-selection boundary and a
test-only probe for upstream `vectorize_loops` (#215, #217). The complete
131-model measurement found no semantic failures; five MIR programs changed
and two log-density values moved by one ULP. Production vectorization remains
off, and the existing C++ reroll pass remains enabled.

### More parameter-dependent programs lower

Parameter-dependent regions now handle data-sized empty outputs, rows in size
expressions, integer assignments and functions, logical and comparison
operators, multidimensional integer-array reads, literal integer arrays,
`rep_vector`, and data-dependent `break` and `continue` (#223). Declared but
uninitialized locals and zero-width data input also lower correctly.

### The R runtime cache is keyed by release

A runtime downloaded by an older version of the R package survived a
package upgrade and kept being loaded until a symbol went missing, with
nothing saying an update was due (#220). The cache path now includes
the release the package pins, so an upgraded package misses the old
cache and the ordinary "run `stanli_install()`" message takes over.
`stanli_install()` prunes runtimes nothing looks for anymore, and its
`version = "latest"` escape is gone: a runtime outside the pinned
directory cannot be found, so fetching one only makes sense together
with `STANLI_RUNTIME`. Reported by @StaffanBetner.

## 0.9.2

### The runtime the R package binds exists again

0.9.1's notes described per-chain write-array streams, but the C-ABI
half (`stanli_wa_seed_chain`, #207) merged after the tag. Builds of the
R package from main -- which is what R-universe serves -- paired a
bridge that binds the new symbol with the pinned v0.9.1 runtime and
failed to load with `missing symbol in stanli library:
stanli_wa_seed_chain`. The symbol ships in this release's runtime and
the pin moves with it. Reported by @StaffanBetner, who also moved the
install docs to lead with R-universe (#204).

### More of the language lowers

A batch of syntax-coverage fixes from @andrjohns (#205), joining
0.9.1's conditional container sizes (#185) and transformed-data
`diag_matrix` (#186): `tcrossprod`, `rep_row_vector`, `while` loops
(bounded by data, unrolled like `for`), UDF locals whose writes are all
skipped at runtime, row-range reads `A[i, lo:hi]`, assignment through
an index vector `x[idx] = rhs` and through row/column index pairs
`M[I, J] = rhs` (repeats resolve last-wins, as CmdStan's assign does),
and unfoldable data-only conditions compile to islands instead of
failing. Landing the batch against the pinned stanc3, which inlines
user functions itself, also taught size expressions to evaluate `sum()`
over an int local built by element writes: an indexed write of an
observed value into an observed container propagates the observation,
and a materialized fill observes the values the slot really holds.

### Islands, continued

Two follow-ups to 0.9.1's island passes. Island register programs are
compacted before the adjoint generates -- dead initializer fills, copy
aliasing, and renumbering away unreferenced registers, table-driven
over the whole instruction set, deleting the ODE-specific version of
the pass. And the generated island adjoint no longer accumulates
partials into data arguments (47% of density arguments across the five
affected islands), a purely backward win with structural value
identity.

### Fixes

The legacy Powell `algebra_solver` (added for the mother model in
\#207) kept its system functor in a temporary while Stan's adapter
defers a reverse-pass callback that references it; the system now
outlives the complete Jacobian sweep (#209).

### Build

The stanc3 compiler is built from a pinned source SHA instead of
fetched from the nightly release (#208), which is replaced in place and
so cannot identify the compiler bytes a release used.

## 0.9.1

### A pass for the lanes re-rolling cannot see

Re-rolling needs a template that repeats with a fixed period, adjacent
and in phase, which is not how most graphs present their per-observation
work. A new pass (`runtime/src/partition.cpp`, #190, #191, #192) asks
where a lane *ends* instead: at a target term or an element store,
reaching back over the ops whose values never leave it. Lanes found that
way need not be adjacent, so they are bucketed by a structural
fingerprint that ignores which slot a lane reads, and each bucket is
rewritten in place of its first lane as gathered vector ops, a density
whose per-lane outcomes are concatenated into the layout its vector
kernel unpacks, a store with a row-reduction tail, or a multi-template
bucket split at whatever writes into the middle of it.
`state_space_stochastic_level_stochastic_seasonal` falls from 1,375 ops
to 19 (2.29x per gradient), `Mth_model` from 1,563 to 35 (1.63x),
`Mh_model` from 1,542 to 18 (1.53x), `Mtbh_model` 1.43x, and
`Survey_model` from 1,427 to 9. One arm rewrites a chain rather than
widening it: the ordinal IRT idiom of scalar product, threshold
subtraction, and cumulative sum feeding a categorical draw is one row of
a `categorical_logit_glm_lpmf`, recognized by dataflow and emitted per
item, which takes `gpcm_latent_reg_irt` from 34,634 ops to 91 (6.5x) and
`grsm_latent_reg_irt` 6.3x. Two measured pessimizations live in the cost
model rather than in the shipped graph: fusing lanes that are identical
down to their immediates re-expands what CSE just collapsed, and a
density with no elementwise kernel trades one vectorized call for W
recorder calls. `STANLI_NO_PARTITION=1` disables the pass.

### Repeated terms collapse before they are evaluated

An unrolled capture-recapture model emits one Bernoulli term per capture
occasion per individual, and most of them are bit-identical: `Mh_model`
has 685 copies, `Mb_model` 786, each a kernel call and a tape entry on
every leapfrog step. Re-rolling leaves them alone, since a target term
has no op consumer. Local value numbering now merges them (#184), made
safe for a graph of mutable buffers by versioning every write and by
letting only an op whose output slot is written once in the whole graph
be the survivor. `Mt_model` goes from 1,062 ops to 70 (16.1x per
gradient), `Mh_model` gains 1.39x, and `gpcm_latent_reg_irt` enters the
partition pass with 34,634 ops rather than 61,612. Placement was
measured: before re-rolling it destroys the periodicity re-roll matches
on, and it runs after lane partitioning, whose lanes it would otherwise
leave in pieces. `STANLI_NO_CSE=1` disables it.

### Kernels stop replaying stan-math, again

The mixture kernels compute closed-form partials instead of building a
nested autodiff tape per element (#184): `normal_mixture` and the
`low_dim_gauss_mix` pair are 45-47% faster, `ldaK2` 46%, `ldaK5` 26%,
with the `log_sum_exp` family bitwise unchanged. Dirichlet, both
`multi_normal` parameterizations, and the tail GLMs stop building that
tape twice per gradient (#189), which is worth 1.28x on
`multi_occupancy` and 3-4% on `hier_2pl`, `gpcm_latent_reg_irt`, and
`ldaK2`; the var-arithmetic densities that drift under seed-then-scale
keep both tapes, so nothing moves for them. The forward `log` runs a
packet at a time in the same change, and the binomial family gains
elementwise forms that cost per element what their summed forms do
(#192), which is what lets `Survey_model`'s bucket fuse at all. `pow`
also joined re-rolling's widenable set (#184): `dogs_hierarchical` -69%,
`dogs_nonhierarchical` -56%.

### ODE callbacks and Jacobian rows

Stan Math's ODE outputs are already precomputed-gradient nodes connected
to the active inputs, so each Jacobian row is now harvested by chaining
its own output node instead of running a nested reverse sweep across
every sibling output, and effect-free scalar right-hand-side bytecode is
compacted at load time (#188). Seven rotating matched Release medians:
`lotka_volterra` 78.5 -> 60.9 us per gradient, `soil_incubation`
101.4 -> 82.1 us, `one_comp_mm_elim_abs` 663.9 -> 629.8 us, with LP and
every gradient component byte-identical to the parent build.

### print and reject are not fusable

Re-rolling's op-match blocklist was missing `OP_PRINT` and `OP_REJECT`,
which constant folding and islands both refuse (#181). The hoist arm
collapsed N per-lane prints into one, and straight-line prints with
distinct literals deduped to the first, because op matching does not
compare the literal payload. Both opcodes now refuse fusion.

### A bare generated-quantities container reports NaN, not 0

Graph-path `write_array` zero-filled the elements of a bare generated
quantities container that indexed assignment never wrote, where CmdStan
and stanli's own MIR interpreter leave them NaN (#182); the cross-path
harness flagged the split. A partially assigned bare container now
reports NaN in the untouched positions, which is a visible change for any
model that has one. The integer arm already used its own sentinel.

### Full-extent store chains stop copying

`normal_mixture_k` carried 1,699 full-extent `SET_SLICE_INPLACE` copies
per gradient, 16.6% of its profile: re-rolling's store fusion forces
the store form whenever the destination is rewritten later. A pass
after in-place conversion renames the readers onto the value slot when
the value has a single reader and the next write is a covering
destructive store (#195). 7.5-8% per gradient on that model, byte
identical output on all 120 corpus models, and only its graph changes.

### Corpus

The full posteriordb benchmark was re-measured on the merged stack
(#187, #193). The median per-gradient speedup against CmdStan moves from
2.17x to 2.91x, 116 of 119 measured models are at or above parity (up
from 104), and nothing regressed by more than 10%. The whole sub-parity
tail is the three ODE models, at 0.87x, 0.90x, and 0.90x. The tables in
`docs/benchmarks.md` and the headline numbers in the READMEs and the demo
page carry the new run.

### Generated quantities use Stan's generator

The caller-owned write-array stream still used `boost::ecuyer1988` after the
sampler moved to Stan's `rng_t`. It now follows BridgeStan's public contract:
Stan's current engine initialized with `create_rng(seed, 0)`. This changes the
generated-quantities sequence produced by a fixed seed, while keeping streams
reproducible and independent. The direct write-array oracle now compares RNG
columns as well as deterministic columns. Sampling frontends give each chain
a fresh `create_rng(seed, chain_id)` write-array stream, so a later chain no
longer depends on how many rows an earlier chain produced. As before, these
postprocessed rows do not preserve CmdStan's already-advanced sampler RNG state
across transitions.

### The stanc3 mother model runs

The 826-line compiler stress model now joins the language oracle, with the two
uninitialized solver inputs in its compile-only upstream source assigned so it
can execute. Supporting it adds the legacy Powell `algebra_solver`, the
semantically empty zero-job `map_rect` case, and mixed multidimensional reads
and partial writes. All three deterministic points compare the log density,
196 gradients, and all 899 write-array values with CmdStan, including its 60
generated-quantities draws.

### Compatibility

Fusion and packet arithmetic change the order of some reductions, so
gradients can differ from 0.8.5 in their last bits. Every change here
replayed the full CmdStan reference corpus with its worst line unmoved,
and the committed verification record stands at 118 of 120 models
verified, 41 of them bitwise, worst relative deviation 2.6e-12. The worst
deviation any pass introduces against the untransformed graph is
5.99e-13, from the IRT GLM synthesis.
`STANLI_NO_PARTITION=1` and `STANLI_NO_CSE=1` restore the pre-pass
graphs when a difference needs attributing.

## 0.8.5

### Wasm builds of the R package bundle their runtime

`stanli_install()` cannot run in a browser: github.com serves release
assets without CORS headers, so webR users had to fetch the runtime by
hand (#163). A configure script now detects an Emscripten cross-build
and bundles the matching runtime into the package, where
`stanli_runtime_path()` finds it before looking at the cache.
`stanli_install()` reports the bundle instead of downloading. Native
builds are unchanged; the script exits before doing anything on a
non-wasm compiler. Suggested by @StaffanBetner.

### Generated quantities compile instead of interpreting

Five more shapes move from the per-draw interpreter onto the
forward-only write_array graph: scalar RNGs (normal, lognormal,
uniform, bernoulli, poisson_log, binomial, categorical), vector
reductions, and min/max (#170, #172, #173, #174, #175). Models whose
GQ blocks were interpreter-bound speed up accordingly: Mh_model rows
went from 869 to 12.5 us, another from 330 to 3.4 us.

### Gradient-path work

Island adjoints pack into a dense file (#168), scalar categorical
probability gets the selected-probability pullback instead of a
nested tape (#169), and compiled ODE callbacks seed mixed scalar
inputs directly into the register file instead of staging copies of
y and theta (#171).

## 0.8.4

### Three kernels stop replaying stan-math

Performance work from three directions, each replacing a nested
autodiff replay with the pinned pullback formulas over doubles, with
bitwise value and gradient coverage against the replay it replaces
(#160, #161, #162). Symmetric eigendecompositions computed in the
forward sweep are reused in the backward one: kronecker_gp goes from
289.0 to 185.7 us per gradient, 0.75x to 1.17x against CmdStan. ODE
solves keep the initial-state and parameter activity types the
lowering already knew instead of promoting both when either is active:
one_comp_mm_elim_abs gains 1.09x. And the single-observation
multi_normal_cholesky shape gp_regr uses gets a native pullback:
6.05 to 4.20 us per gradient, 0.77x to 1.12x against CmdStan.

### A runtime for webR

`stanli_install()` on webR resolves the asset name
stanli-runtime-emscripten-wasm32.tar.gz, which no release published, so
the R package installed from r-universe could never find its runtime
(#163). The release pipeline now builds libstanli.so as an Emscripten
side module and attaches it to every release beside the five native
runtimes. The build is pinned to the Emscripten version the current
webR release uses (webR 0.6.0, Emscripten 4.0.8) because side modules
are not binary compatible across webR's Emscripten bumps; CI loads the
artifact into webR itself and resolves the bridge's symbols. The full
flow -- the r-universe package, log_prob_grad, NUTS -- was verified
against webR 0.6.0 by hand.

### Stan compiles inside webR

`stanli_model(code = ...)` works on webR now, with no V8 package. webR
already runs inside a JavaScript engine, so the webr support package's
eval_js() is the engine the V8 package would otherwise provide: the
bundled stanc.js defines stanc() in the worker's global scope once per
session, and source and MIR travel through the shared Emscripten
filesystem, so nothing is escaped into a JavaScript literal and no
marshalling limit sees a megabyte of MIR. A native stanc still wins
where one can run; under webR the binary probe returns early, which
also silences a Sys.which warning webR printed on every first compile.

## 0.8.3

Five wrong-number fixes, every one found by testing machinery that did
not exist two weeks ago, and the machinery itself so the next class of
these is caught on the pull request that makes it.

### target += of a container adds sum(e), not its first element

`target += f;` on a `vector[3]` added `f[1]`: lp came out as the first
element and the other gradients were zero, silently. Stan defines
`target += e` for a container as adding `sum(e)`. No posteriordb model
writes the bare form -- they all go through an `_lpdf`, which returns a
scalar -- and the signature-directed conformance sweep generates calls,
never this statement, which is how it survived. Found by the new model
census over stanc3's own 1,231 integration models; on that suite's
`increment-target.stan` the fixed lp and every gradient are byte
identical to CmdStan's. The register machine's refusal of the same
statement inside parameter-dependent control flow is lifted too, and the
term-reduction path now rejects a non-scalar term outright so this class
is loud forever.

### A row_vector answers rows = 1, cols = n in every engine

The MIR interpreter read orientation off storage rank, so every
`row_vector[n]` answered `rows = n, cols = 1`. Through transformed data
that reaches the log density: a target term built from `rows(rv)` graded
31 where CmdStan gives 13. Invisible to the conformance sweep, because
int-returning signatures have no real-bearing lane; the new cross-path
harness caught it on its first sweep. The graph side carried a latent
copy of the same logic, proven unreachable and deleted. The fix
initially removed a fallback that `size()` of a scalar int data value
still needed -- caught within days by the census ratchet's tally and
restored, so no release ever shipped without it.

### sqrt and pow adjoints at exactly zero

Both computed their backward unconditionally, so an output of exactly
zero produced NaN gradients where CmdStan is finite: `dout / (2 * out)`
for sqrt, `0/0` and `log(0) * 0` for pow. accel_gp -- a real posteriordb
posterior -- hit the sqrt case whenever its GP length scale made a
spectral density underflow to zero: lp bitwise equal to CmdStan, two
gradients NaN, at two of three evaluation points, with the recorded
reference sitting on the clean one. The fixes mirror stan-math's own
guards, and the audit behind them swept every rev-mode boundary guard in
stan-math: exactly four exist, stanli now matches all four, and the
kernels deliberately unguarded on both sides are named in the pow fix so
the next boundary report starts from a map.

### The oracles remember

The corpus replay now evaluates every model at all three deterministic
points against full CmdStan references -- 800,674 compared values, up
from 146,534 -- with a crash at any point a named, blocking failure. The
conformance sweep and the new census both ratchet against checked-in
baselines: coverage lost blocks with the case named, coverage gained
records. A cross-path harness compares the op graph, the MIR
interpreter, and the register machine against each other on every
fixture, bitwise, with known divergences in a ledger that requires a
cause. The test suite runs under AddressSanitizer on every pull request.
And the one remaining engineered disagreement -- write_array sums that
cancel to zero, where stanli reduces sequentially for lp parity while
CmdStan lets Eigen vectorize -- is gated at 1e-15 absolute with its
mechanism written down, so the sweep is green with nothing swept under
it.

## 0.8.2

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
