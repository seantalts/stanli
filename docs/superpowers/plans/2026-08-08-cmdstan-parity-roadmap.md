# Competing with CmdStan: the seven-item roadmap

**Premise.** The corpus fight is essentially won: median 2.07x per
gradient, 93/119 at or above CmdStan, ~20x time-to-first-draw, 118/120
differentially verified, 71/72 densities. What decides the competition
from here is *not* posteriordb. It is three things posteriordb does not
measure:

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

## 5. Native adjoint program, and the scoped tail fixes

The perf class-changer. Full design:
[2026-08-08-native-adjoint-program.md](2026-08-08-native-adjoint-program.md).

Alongside it, the tail fixes the profile already names:

- **Slice writes in place.** `Mtbh_model` (0.45x, the corpus floor)
  spends 36% in `OP_SET_SLICE_STRIDED` — 146 calls moving 106,580
  elements. The in-place rule rewrites element writes and not slice
  writes, so a model filling a matrix column by column copies the whole
  matrix per column. Flagged "Open" in docs/benchmarks.md.
- **Row-wise `log_sum_exp`** closes `ldaK2`/`ldaK5` (0.71x). The K=2
  case is already closed by the elementwise-lp fusion.
- **Kernel-bound stragglers**, in the shape of the `diamonds` and
  `prophet` fixes: `Mt_model` (62% in a scalar `bernoulli_lpmf`),
  `gp_regr` (55% in `multi_normal_cholesky_lpdf`), `kronecker_gp` (38%
  in an eigendecomposition).
- **Two loose threads.** `lotka_volterra` is 0.61x per gradient yet blows
  the 900 s sampling cap CmdStan clears in 6.2 s — untraced, and
  `tools/sampler_trace.py` is the tool. `sir`'s benchmark probe point
  lands where its ODE solution dips to -4.4e-10; move the point and the
  corpus goes 119/120 verified.

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

- **CRAN shim** (already roadmap item 2 in the README). All five wheels
  are built and published by `.github/workflows/wheels.yml`.
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
