# Changelog

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

- An npm package, `stanli`: `compile()` and `sample()` over a worker
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
