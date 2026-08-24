# Competing with CmdStan: the seven-item roadmap

**Premise (2026-08-08 framing; corpus figures refreshed 2026-08-24).** The
corpus fight is essentially won: median 2.18x per gradient, 109/119 at or
above CmdStan, ~20x time-to-first-draw, 118/120 differentially verified,
71/72 densities. What decides the competition from here is *not*
posteriordb. It is three things posteriordb does not measure:

1. **Workflow.** A Bayesian workflow tool runs four chains and reports
   R-hat, ESS and divergences. stanli runs one chain and reports draws.
2. **Language reach.** posteriordb is 2019-era Stan. A 2026 model uses
   `offset`/`multiplier`, the variadic `ode_*` interface, `reject`,
   `reduce_sum`, `sum_to_zero_vector`. Each of those is a hard lowering
   failure today, and a model that does not lower has no speedup.
3. **Threading.** CmdStan's real-world pitch is 4 parallel chains x
   `STAN_THREADS` `reduce_sum`. stanli has no threading at all.

The ordering below is by (value / cost), and each item is written so it
can be picked up cold.

---

**Status, 2026-08-08:** items 1, 2 and the solver half of 3 are done and verified (30/30 tests,
20/20 transforms bitwise vs CmdStan, 119/119 corpus models unchanged
against the stored references). Items 3-7 are as written.

**Status, 2026-08-24:** item 5's native adjoint program and the six scoped
optimizations named there have landed. Its current ratios below come from the
refreshed full-corpus run; the before/after arrows remain the historical
targeted A/B medians that attribute each change.

## 1. Multi-chain, sampler stats, inits, diagnostics — DONE

**Why first:** it is the cheapest item on the list and the one that
changes what stanli *is*. Nobody computes R-hat off one chain, so today
a user cannot even check whether a stanli run converged without exporting
draws to another tool.

**What exists already, unexposed:** `SamplerStats` (nuts.hpp) collects
CmdStan's seven columns and only `stanli_run --sampler-stats` reads it;
`stanli_sample_stream` is in `capi.h` and unwired in Python. `deps/stan`
ships `stan/analyze/mcmc/` — rank-normalized split-R-hat, bulk/tail ESS,
MCSE — which is exactly what `stansummary` uses, so the diagnostics are
reuse, not new math.

**Scope:**

- `stanli_sample_multi` in the C ABI taking an options struct (chains,
  seed, warmup, samples, thin, delta, max_depth, save_warmup, init
  radius, explicit inits, threads) and returning draws + the seven
  sampler columns per chain.
- Chain `c` uses `create_rng(seed, c + 1)` — CmdStan's chain-id
  convention, so a matched seed means a matched stream per chain.
- **Diagnostics** (`runtime/src/diagnose.cpp`), Betancourt's list:
  divergences, max-treedepth saturation, **E-BFMI** (his own diagnostic;
  flag below 0.3), rank-normalized split-R-hat, bulk and tail ESS, MCSE.
  In C++ so the CLI, Python and the browser all get the same numbers.
- Python: `chains=`, a `summary()` returning the stansummary table, a
  `diagnose()` returning Betancourt's checks in his words, arviz export.
- CLI: `--chains`, `--num-threads`, a summary block.

**The threading hazard, stated up front:** stan-math's AD stack is a
plain static unless `STAN_THREADS` is defined, in which case it is
`thread_local`. Legacy ops and islands build *nested* var tapes, so two
chains in two threads share and corrupt one stack. Threads are therefore
gated on a build-time capability (`stanli_thread_safe()`), and the
default is sequential. Sequential multi-chain is still the whole
workflow win: four chains of eight schools is 80 ms against CmdStan's
5 s compile.

---

## 2. `offset`/`multiplier`, the missing transforms, `reject` — DONE

Landed except parameter-dependent control flow, which is the last
bullet below and is really item 5's island v2 rather than a transform
problem. `harnesses/transform_sweep.py` is the new oracle.

**Why second:** cheap, and it is what modern hand-written and
brms-generated models are made of.

- **`offset`/`multiplier`** already parse into the MIR
  (`mir.hpp:32`) and then hit `fail("unsupported parameter transform")`
  in `lower.cpp:1761`. It is an affine transform with a constant
  Jacobian. This is the single highest-value language fix on the list.
- **`unit_vector`, `corr_matrix`, `cov_matrix`, `cholesky_factor_cov`,
  `sum_to_zero_vector`** (brms emits the last one now). `corr_matrix`
  and `cholesky_factor_cov` are also why `lkj_corr` and the wisharts
  currently need a transformed parameter to be reachable at all
  (docs/coverage.md).
- **`reject` and `print`** statements — not lowered. A censored or
  hurdle model that guards its support with `reject` fails outright.
- **Parameter-dependent control flow** — `lower.cpp:394` refuses boolean
  operators on parameters, and `While` is not lowered. The path is
  island v2: `RhsProgram` already has `JZ`/`JMP` and `IslandProg` already
  has densities; they were merged into one `Program` (program.hpp), so
  the instructions exist. Lowering compiles such a region straight to an
  island instead of failing.

---

## 3. Modern `ode_*` and the other solvers — DONE (solvers), open (other solvers)

The four `ode_*` solvers and their `_tol` forms landed, verified against
CmdStan by `harnesses/ode_sweep.py`. What is still open from this item is
the rest of the paragraph below: `algebra_solver`, `solve_newton`,
`solve_powell`, `integrate_1d`, and the DAE family.

Only the deprecated `integrate_ode_*` family lowers (`lower.cpp:1628`).
Current models use the variadic `ode_rk45`/`ode_bdf`/`ode_adams`/
`ode_ckrk` interface. Table stakes, and the register machine that made
ODEs 29-39x faster already exists.

Same shape, same machinery: `algebra_solver`/`solve_newton`/
`solve_powell`, `integrate_1d`, and the DAE family. All of them are
"a user function that stays callable at runtime", which is exactly what
`RhsProgram` was built for.

---

## 4. Pathfinder, optimize, laplace — PARTIAL

**L-BFGS landed** (`Model.optimize()`), verified against a posterior
whose mode is known in closed form. It returns the posterior MODE and
refuses CmdStan's `jacobian=0` default rather than returning the wrong
quantity under that name -- stanli folds the Jacobian into the graph at
lowering time. `lower.cpp` already collects `jac_slots` separately, so
making that flag real is the next piece.

**Pathfinder is not landed.** `ExecutorModel` now satisfies enough of
the model concept that stan's single-path service compiles and runs,
but its parameter writer is never called and the draws come back empty;
that is one debugging session, not a design problem. Multi-path is a
harder blocker: it uses `tbb::parallel_for` and this build stubs TBB
out, so it does not link. **laplace_sample** and standalone
**generate_quantities** are untouched.

`deps/stan/src/stan/services/` ships `pathfinder/`, `optimize/`,
`laplace_sample`, and `diagnose/`. They are drivers over a model's
`log_prob_grad`, which `ExecutorModel` already provides — this is
mostly adapter and C-ABI work, not algorithm work.

Pathfinder matters twice over: it is a deliverable on its own, and it is
the modern default for *inits*, which is item 1's `inits` parameter
pointed at something better than uniform(-2, 2).

Standalone `generate_quantities` belongs here too: the write_array graph
and the per-draw interpreter both exist, and it needs only a
draws-CSV-in entry point.

---

## 5. Native adjoint program, and the scoped tail fixes — DONE (named work), open (remaining tail)

`gen_adjoint` and the six named tail optimizations have landed. Full native
adjoint design:
[2026-08-08-native-adjoint-program.md](2026-08-08-native-adjoint-program.md).

Unless a measurement is explicitly labeled a targeted A/B, the ratios below
are CmdStan/stanli from the 2026-08-24 full corpus, whose gradient cells are
warmed arithmetic means.

- **Slice writes in place — LANDED.** The original profile found 146
  `OP_SET_SLICE_STRIDED` calls copying a 730-value matrix. The targeted A/B
  median for `Mtbh_model` fell from 106.5 to 47.4 us/gradient (0.43x to 0.95x
  CmdStan). After the later Bernoulli specialization, its current full-corpus
  warmed mean is 27.0 us/gradient, or 1.59x CmdStan.
- **Packed row-wise `log_sum_exp` — LANDED.** The targeted A/B medians fell
  from 154 to 94 us for `ldaK2` and from 6.82 to 3.70 ms for `ldaK5`. Their
  current full-corpus ratios are 1.05x and 1.52x CmdStan, respectively.
- **Native Bernoulli forwards — LANDED.** The targeted A/B medians were 30.6
  to 19.2 us for `Mt_model`, 113.2 to 57.3 us for `Mth_model`, and, after the
  slice fix, 47.4 to 26.8 us for `Mtbh_model`. Their current full-corpus ratios
  are 1.05x, 1.65x, and 1.59x CmdStan.
- **Native symmetric-eigen pullbacks — LANDED.** Retaining each
  eigendecomposition moved `kronecker_gp` from 289.0 to 185.7 us/gradient in
  the targeted A/B. Its current full-corpus ratio is 1.18x CmdStan.
- **Native `multi_normal_cholesky` partials — LANDED.** Retaining the exact
  active-Cholesky partial matrix moved `gp_regr` from 6.05 to 4.20 us/gradient
  in the targeted A/B. Its current full-corpus ratio is 1.11x CmdStan.
- **Mixed ODE activity types — LANDED.** Removing the inactive initial-state
  sensitivity moved `one_comp_mm_elim_abs` from 699 to 639 us/gradient in the
  targeted A/B; its current full-corpus ratio is 0.75x CmdStan. The fully
  active `lotka_volterra` and `soil_incubation` shapes do not benefit from this
  specialization and currently sit at 0.53x and 0.59x.
- **The remaining tail.** `lotka_volterra` now completes sampling in 10.00 s
  versus CmdStan's 6.24 s; the earlier timeout claim is no longer reproducible,
  although its 0.53x gradient ratio still points to solver/RHS dispatch.
  `iohmm_reg` is 0.65x CmdStan in the current full warmed-mean run. Its 4.74x
  generated-adjoint result was a targeted comparison against islands disabled,
  not a CmdStan speedup; stored loop bodies do not remove per-executed-
  instruction dispatch. The other sub-parity rows are the two
  latent-regression IRT shapes at 0.74-0.79x, `multi_occupancy` at 0.79x,
  and three near-parity rows at 0.89-0.94x. `sir`'s benchmark probe point still
  lands where its ODE solution dips to -4.4e-10; moving the point would take
  the corpus to 119/120 verified.

---

## 6. `reduce_sum`, serial then threaded

`reduce_sum` and `map_rect` are not lowered at all, so a model *written*
with them will not run even single-threaded. Serial lowering (inline the
partial sums, one term per slice) gets them running and is worth doing
on its own.

Threading them is the follow-on, and it depends on the same
`STAN_THREADS` decision item 1 forces. Worth noting: an interpreter over
large vector ops is unusually well placed to thread data-parallel kernels
*without* `reduce_sum` semantics at all — the op boundary is already the
parallel boundary. That is a stanli-shaped answer to a CmdStan-shaped
problem, and it should be measured before assuming `reduce_sum` is the
only route.

---

## 7. R/brms and the wasm density pack

- **CRAN shim** — DONE, as `r/`. Not a shim over the Python package in
  the end but a package in its own right: an R-level API, a dlopen
  bridge to the C ABI, and stanc3 as JavaScript through V8, which is how
  rstan carries a Stan compiler on CRAN and the only form of the
  compiler CRAN can take. `R CMD check --as-cran` is clean but for the
  permanent "New submission" NOTE. The runtime is downloaded rather than
  bundled, so the same `v*` tag that publishes the wheels also attaches
  five platform runtime tarballs to the release, and the C ABI carries a
  layout version the bridge refuses to mismatch. **Still to do:** the
  r-universe registry entry (one file in a separate repo) and the CRAN
  submission itself, which is a web form and a confirmation email by
  policy and cannot be automated.
- **A brms backend** is the bigger play. R is half of Stan's user base,
  and brms-generated code is a natural stress corpus that exercises
  precisely the gaps in item 2 — `offset`/`multiplier`,
  `sum_to_zero_vector`, `reject`.
- **The wasm density pack** is proven end to end (a `SIDE_MODULE` spike
  calling back into the main module) and takes the core download to
  ~0.69 MB gzipped. docs/density-pack.md has the design.
- **Browser multi-chain** is one worker per chain, no SharedArrayBuffer
  needed, and it lets the demo page report R-hat honestly.
- **Publish the npm package.** `js/` holds a 0.1.0 tarball. The browser
  is the one channel CmdStan structurally cannot follow stanli into.

---

## What is deliberately not on this list

- **Tuples and complex numbers.** Absent from the MIR types entirely, and
  a genuinely large lift. brms emits neither. Defer, but say so in
  docs/coverage.md rather than leaving it silent.
- **`gaussian_dlm_obs`.** Needs `Op::in` > 6, which costs bytes in every
  `Op` and every `KernelCtx` in every model, for one density.
- **varmat/SoA kernels.** Real, and already README roadmap item 3. It
  raises the ceiling on dense-op models rather than fixing a floor, so
  it sits behind the items above.

One honesty note for `docs/benchmarks.md` when that work happens: the
comparison is against *default* CmdStan. A user who builds with
`stanc --O1` gets a faster CmdStan, and competing eventually means
benchmarking against that configuration too.
