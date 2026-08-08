# Changelog

## 0.3.0

- Stan runs in the browser. stanc3 compiled to JavaScript through its
  own js_of_ocaml target, this runtime compiled to WebAssembly through
  Emscripten, and nothing on a server: a model is compiled and sampled
  in the page. 118 of the 120 corpus models replay through the WASM
  build under Node against the same recorded CmdStan values the native
  build is checked against, generated-quantities columns included
  (`nn_rbm1bJ100` is the exception, and it wants more than the 4 GB a
  wasm32 heap can address). The demo is at
  [seantalts.github.io/stanli](https://seantalts.github.io/stanli/):
  presets, chains sampling simultaneously in one worker each, live trace
  and histogram plots while NUTS runs, split-Rhat and effective sample
  size, and a CSV of the draws. The payload is 1.34 MB of runtime plus
  0.43 MB of compiler, gzipped; a page that ships precompiled MIR never
  loads the compiler at all.

- An npm package, `stanli`, wrapping that: `compile()` and `sample()`
  over a worker pool sized to the hardware, with an `onLive` callback
  for streaming draws. Published on `npm-v*` tags through npm trusted
  publishing.

- A Windows wheel. `win_amd64` joins the four existing platforms, built
  under mingw-w64 (stan-math does not build under MSVC, which is why
  RStan ships through RTools), exporting the C ABI through a `.def`
  file. It bundles the release `stanc.exe` and drives it as a
  subprocess rather than embedding the compiler, which waits on opam's
  native Windows support.

- write_array reached the C ABI: `stanli_wa_n_columns`,
  `stanli_wa_column_name`, `stanli_wa_seed`, `stanli_wa_row`. Every
  binding gets the columns CmdStan would write, in CmdStan's order,
  including the models whose generated quantities are interpreted per
  draw because the graph cannot express them.

- The benchmark table on the PyPI page renders as a table again. The
  marker that stamps generated numbers into the page shared its line
  with the table header, and a line opening `<!--` opens a raw HTML
  block that runs to the `-->`, so PyPI's renderer swallowed the header
  and printed every row as literal pipes. `tools/gen_docs.py --check`
  now renders both READMEs with readme_renderer, the library PyPI
  itself uses, and fails if a table does not come out as one. `twine
  check` never caught this: the page rendered, it just rendered wrong.

- The sampler draws from CmdStan's generator now. It built
  `boost::ecuyer1988` from the seed while CmdStan builds
  `boost::random::mixmax` as `(0, 1, seed, chain)`, so the same seed
  named unrelated streams and any sampling comparison was comparing two
  different draws as much as two engines. `run_nuts` calls
  `stan::services::util::create_rng` and draws the initial point the way
  `stan::io::random_var_context` does. Note that a given seed now
  produces different draws than 0.2.0 did; any seed is as valid as any
  other, but a run pinned to a seed will not reproduce byte for byte.

- Initial points are accepted the way CmdStan accepts them, which fixes
  `lotka_volterra`'s sampling timeout. CmdStan checks a candidate by
  evaluating the log density on doubles and then its gradient; we only
  ever ran the second. That matters for an ODE model, because the value
  path solves the states alone while the gradient path solves the coupled
  state-plus-sensitivity system, and at a solution grazing zero the two
  disagree in sign (measured on the point in question: -1.81e-05 against
  +5.33e-06, so log(z) is NaN for one and finite for the other). We were
  accepting starting points CmdStan rejects, and at the corpus seed that
  meant a chain that never left a bad region -- lp -1260 against a
  typical set near -14, and 87x the leapfrogs, which is the whole
  timeout. `Executor::forward_value_only()` is the double path, and
  OP_ODE is the one kernel that does less work in it.

- A draw whose generated quantities throw is written as nan and sampling
  continues, which is what CmdStan does. `stanli_run` used to abort and
  print nothing, so one bad `lognormal_rng` on a marginal ODE solution
  discarded every draw of an otherwise good chain.

- 34 scalar math functions and 14 more distributions. `lgamma`, `log1p`,
  `Phi`, `inv_Phi`, `erf`, `expm1`, `digamma`, the trig and hyperbolic
  families, `floor`/`ceil`/`round`, `inv`/`inv_sqrt`/`inv_square` and the
  rest now work on the parameter path and in transformed data and
  generated quantities, taking coverage of stanc3's scalar function list
  from 13 of 103 to 47 of 103. The distributions are `chi_square`,
  `inv_chi_square`, `scaled_inv_chi_square`, `frechet`, `gumbel`,
  `loglogistic`, `pareto`, `pareto_type_2`, `rayleigh`, `skew_normal`,
  `von_mises`, `exp_mod_normal`, `beta_proportion` and
  `skew_double_exponential`. Every one is bitwise identical to CmdStan,
  checked by `harnesses/fn_sweep.py`, which generates a model per
  function from stanc3's own signature list and compares against the
  same reference driver the corpus uses.

- The library is 19.1 MB installed, up from 13.8. Distributions are what
  cost: each is instantiated once per activity mask, twice for propto and
  again for the elementwise form, about 630 KB apiece, which is what a
  precompiled library pays so that no model has to be compiled. The long
  tail of them now takes a smaller form -- with propto off no terms are
  dropped, so one binding covers every mask -- which returned 4.4 MB of
  the 10.5 the additions cost, and leaves the distributions models
  actually use running exactly as fast as before. The 34 scalar
  functions cost 0.03 MB between them.

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
  cache-blocking the accumulator, swapping the loop nesting (both
  slower than what they replaced), and the same partial-stashing on
  `multi_normal_cholesky_lpdf` (a wash, and it would have cost n^2
  doubles of scratch per op).

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
