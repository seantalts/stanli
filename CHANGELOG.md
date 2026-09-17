# Changelog

## Unreleased

- Add `sample_ulam()` to translate total iterations, warmup, cores, control
  settings, and constrained starts into the native CmdStanR-style sampler.
  Integrations no longer need to implement that translation themselves;
  `sample_cstan()` retains its existing interface and defaults.

## 0.14.3

- Configure the optional CmdStanR repository for release-platform R and stanr
  dependency resolution. Version 0.14.2 reached npm before the full release
  stopped; 0.14.3 is the complete release across distribution channels.

- Add `as_cstanfit()` and `sample_cstan()` for native CmdStanR-style fit
  methods and sampler arguments without cmdstanr or rstan. Stored-draw
  summaries, diagnostics, and LOO survive serialization without a runtime,
  with methods delegated to the installed package for future fixes.
- Accept extra initialization fields in the CmdStanR-style sampler, and
  preserve native fit conversion for models without recorded outputs.

- Preserve distinct compiled programs when optimizing loops, and avoid
  redundant copies of single region results.
- Support brms COM-Poisson models with checked runtime vector writes, prefix
  reductions, and integer remainder inside parameter-dependent control flow;
  skip unused workspace initialization on early-return branches.
- Reduce constant allocation, inactive derivative work, and unused input copies
  in runtime-control replay; reuse verified workspaces and remove overwritten
  initializers. COM-Poisson sampling now exceeds the 0.8× CmdStan/Stanli target.

## 0.14.1

### Improvements

- Add `as_rfit()` and native RStan-style extraction, summaries, sampler
  parameters, and timings without an RStan dependency. Keep the existing
  optional S4 `as_stanfit()` conversion and ecosystem integrations.

- Speed up categorical, ordinal, multivariate-normal, Dirichlet, Wiener,
  truncated-distribution and GP calculations; reduce loop and preparation costs.
- Document Rethinking, brms, and educational-model support with current-build
  numerical checks, a full timing appendix, and a Rethinking report. Cap
  benchmark sampling at 3x its CmdStan baseline or 900 seconds, whichever is less.
- Add the complete second-edition rethinking `ulam()` corpus: 61 book calls
  plus a hurdle fixture, pinned regeneration, and CmdStan reference checks.
- Add optional `as_stanfit()` conversion with native density, gradient, and
  transform methods, plus LOO moment matching. Preserve draws, diagnostics,
  and timings without C++ model compilation; verify against RStan references.
- Integrate R fits with bayesplot, loo, and tidybayes through optional S3
  methods; preserve posterior conversions and exclude saved warmup by default.
  Add cmdstanr migration and classroom guides, persistence tests, and platform
  CI checks with fresh-session timing.

### Fixes

- Match CmdStan's fixed-covariate GP gradient reduction order and accept
  zero-argument `log2()` and `log10()` constants in portable MIR.
- Replace corpus benchmark heuristics with equal warmup, alternating paired
  trials, median/MAD summaries, and versioned raw records that cannot mix runs.
  Remove timeout-polling delay from short-process wall times.

## 0.14.0

- Add 13 educational models with fixed CmdStan references, complete output
  checks, and separate gradient and end-to-end benchmark tables. All pass the
  recorded posterior and performance gates; Pareto's final five-pair complete
  run is 1.072x CmdStan, while its fixed-point gradient remains slower at 0.64x.
- Load compiler overload sets lazily, share signature lookups, avoid exceptions
  for ordinary constant probes, and prune unused model-only procedures.
- Generate adjoints for forward-only branches when initialization and aliasing
  checks permit; compile integer RNG control and share Poisson, Student-t and
  Bernoulli-logit kernels. Generated quantities skip adjoint generation.
- Buffer CLI CSV formatting while preserving 17-digit output and seeded values.
  The recorded Pareto comparison improves complete-run time by 1.790x. (#367)
- Add Windows x64/arm64 setup CI, preserve embedded compilation, save compiler
  caches before tests, and resolve test Bash without launching WSL. (#360)
- Install the shared sanitizer core with native tools. (#365)
- Collapse the browser sampling diagnostics by default while keeping the verdict
  visible, aligning the NUTS and WALNUTS comparison tables. (#361)

## 0.13.0

### Features and performance

- Lower the expanded loop vectorizer's row stores, empty assignments, and
  elementwise arithmetic without the earlier corpus regressions. Re-roll
  whole container lanes, use dimension-independent indexed stores, and compile
  vector operations as range instructions. (#350)
- Price reductions and absorbed constants when choosing island boundaries;
  the compile-time carver replaces the preparation-time tuner and replay state.
- Use the shipped compilation pipeline in corpus checks and benchmarks, with
  explicit compiler overrides for experiments. Record compiler choices and
  retain raw before/after measurements.
- Evaluate transformed parameters and generated quantities on each sampling
  thread, inside progress reporting, removing R and Python's post-run phase.
- Support RNG calls in transformed data, including nested user RNG functions.
  Model construction accepts a seed; sampling and optimization rebuild models
  under their run seed only when transformed data drew from the RNG. Refresh
  derived dimensions and columns after rebuilding. (#348)
- Share unwritten model data across executors while keeping mutable buffers
  private. Eight MNIST executors use 4.8 GB instead of 10.3 GB. Parse JSON arrays
  directly and reuse GP covariance and Cholesky forward results in pullbacks;
  the recorded gp_regr gradient falls from 6.7 to 2.6 microseconds. (#357)
- Build with `STANLI_NO_STDIO` for R packaging; route debug output through the
  diagnostic sink and grow the preparation profiler instead of aborting.
- Replay every generated builtin and density signature at three CmdStan points,
  including mixed data/parameter arguments, gradients, and outputs. Changed
  source partitions require fresh reference recording.

### Fixes

- Poll an already-pending interrupt before starting sampling threads.
- Avoid software long-double evaluation of `beta_neg_binomial_cdf`, `_lcdf`,
  and `_lccdf` on non-x86 targets; retain x86 promotion for CmdStan parity.
- Restore parameter design-matrix gradients in three GLM families, and active
  `eta` gradients and proportional densities in LKJ kernels. Reject density
  plans whose active arguments the kernel cannot differentiate.
- Match type-dependent `fmax`/`fmin` adjoints for ties and NaNs in runtime control
  flow. Resolve earlier locals' shape queries from their declarations, keeping
  inlined generated-quantities functions on the compiled path.
- Align island interpreter entry points to a cache line. Document reduction
  rounding against a high-precision reference instead of treating every ULP
  distance from CmdStan as a Stanli error.
- Assign the adjoint ODE quadrature result before Stan Math accumulates it.
  (#356)
- Keep `static` on the same line as its field in browser compiler bundles,
  restoring Safari 17 and iOS 17 compatibility. (#347)
- Replace destructor-bearing thread-local runtime objects with executor scratch
  and pooled tape leases, preventing DLL-unload crashes under MinGW. (#352)

## 0.12.0

### Features

- Ctrl-C stops all sampling chains after their current transition in R and
  Python, including RStudio's stop button and Windows. The new C API
  `stanli_sample_multi_interruptible` polls on the calling thread. (#327)
- R and Python use bracket names such as `theta[1,2]` for indexed values. Python
  still accepts dot names for lookup; `Fit` and `OptimizeResult` support
  shape-preserving variable lookup, and `to_arviz()` preserves dimensions.
  (#328)
- Report interpreter fallbacks, with the lowering reason, in R, Python,
  `stanli_run`, and the `stanli_warnings` C API. `STANLI_NO_INTERPRETER=1`
  makes them compile errors. Raise the runtime-control register limit from
  2^16 to 2^20, allowing `hmm_gaussian` and `iohmm_reg` outputs to compile.

### Fixes

- Use live slice lengths in retained-loop reductions, elementwise operations,
  dot products, densities, and gradients. Reject incompatible operand shapes
  instead of silently narrowing them; support runtime submatrix shape queries
  and gathers through runtime-length selectors.
- Match Stan Math's type-dependent `pow` gradients at a zero base, including
  scalar and matrix overloads. This fixes brms `ar(cov = TRUE)` gradients.
- Evaluate `choose` in compile-time integer expressions, fixing brms correlation
  indexing and `logistic_normal` declarations.
- Preserve output columns substituted with constants by `--O1`, including brms
  ordinal models' `disc` parameter. Add LKJ densities to the output interpreter.
- Support Poisson, Bernoulli, binomial, and negative-binomial-2 densities,
  including log/logit forms, in parameter-dependent control flow when outcomes
  and trial counts are compile-time integers.
- Add the nullary constants `log2()`, `log10()`, and `sqrt2()`.
- Unroll data-controlled `while` loops whose locals are sized by loop state,
  fixing brms `unstr()` models. Track block-local integers in statement order
  when selecting runtime-control regions.
- Accept per-row vector intercepts in Bernoulli, Poisson, negative-binomial-2,
  and binomial GLMs; fix binomial GLM partials writing out of bounds.
- Include the model's first exception in initialization failures.
- Resolve shape queries for elementwise expressions, transposes, matrix rows,
  and gathered submatrices, fixing brms category-specific ordinal models.
- Accept bare and quoted non-finite real data (`Infinity`, `-Infinity`, `Inf`,
  `-Inf`, `NaN`). R and browser writers preserve these tokens; R double `NA`
  becomes `NaN`. Integer data still requires integer values, and missing integer
  or logical R data is rejected.
- Add `gp_matern32_cov`, `gp_matern52_cov`, and `gp_exponential_cov`, including
  coordinate, scale, and length-scale gradients. (#320)
- Accept arrays of locations in multivariate normal densities, with elementwise
  pairing or broadcasting, supporting brms `set_rescor(TRUE)`.
- Handle whole-value index nodes and vectorized integer-array column writes in
  the interpreter, fixing CSV output for eight corpus models.

### Performance and verification

- Call the executor's gradient directly from NUTS, removing 12–30% sampling
  overhead on affected models. `bym2_offset_only` falls from 15.2 s to 12.6 s;
  `tools/bench_grad.cpp` now measures the sampler's actual gradient call.
- Add 124 models generated by brms 2.23.0 under `tests/brms/`, with CmdStan
  references replayed in CI. `tools/gen_brms_models.R` regenerates them;
  `s2_com_poisson` remains in `KNOWN_GAPS`.
- Fail corpus replay when a model produces no output row, even if its recorded
  reference has none, closing a gap that hid empty CSV output.

## 0.11.1

- Fix out-of-bounds reads in generated reverse passes for zero-length copies.
- Allocate higher-order backward dispatch buffers per call, fixing gradients
  when islands nest ODE, quadrature, DAE, or other island kernels.
- Store retained-loop history per write, with in-place updates and undo logs.
  Loops no longer require compile-time iteration bounds;
  `STANLI_STRUCTURED_HISTORY_BYTES` and `STANLI_STRUCTURED_MEMORY_PROFILE`
  are removed.
- Cache data-only loop work and control decisions after the first evaluation,
  and compile scalar kernel sequences into one program with a generated reverse
  pass. ctsem at 400 rows drops from 4.4 s to 0.32 s per gradient;
  `STANLI_NO_STRUCTURED_SEGMENTS=1` disables sequence compilation.
- Release dead data-only subtree storage during recording and resolve dynamic
  selectors once per call. ctsem recording memory falls from 1.24 GB to 0.95 GB;
  selector changes improve ctsem gradients by 14% and radon_county by 20%.
- Retain outermost `while` loops by default, plus outermost `for` loops of at
  least 32 iterations containing a `while` or parameter-dependent branch.
  `STANLI_STRUCTURED_LOOPS=1` changes which loops are retained.

## 0.11.0

### Features

- Call pure, value-returning Stan user functions through Python's
  `stanli.Function` and C++'s `stanli::Function`, using source or cached MIR,
  named arguments, overload resolution, and shape-preserving results. Python
  uses typed numeric buffers without JSON serialization.
- Compile large data-dependent loops with parameter-dependent bodies as native
  graph regions, reducing preparation time and memory. Unsupported bodies
  retain the previous unrolling behavior. (#248)
- Support `print`, runtime `reject` messages, all fifteen constrained-parameter
  Jacobian transforms, recursive/data-dependent user functions, mixed integer
  and real arguments, and more math functions in runtime-control regions and
  ODE callbacks. Preserve argument grouping in container reductions.
- Initialize NUTS from single-path Pathfinder draws in Python, R, and the
  browser. Pathfinder initialization is mutually exclusive with explicit
  initial values and skips PSIS resampling. (#303)
- Convert constrained initial values with Python's `Model.unconstrain()`, R's
  `unconstrain()`, the C API, and BridgeStan's unconstrain/initialize entries,
  including bounds depending on earlier parameters.
- Support `reduce_sum` with serial slice execution; threading is not yet
  implemented.
- Add `reverse`, `block`, `to_matrix`, `linspaced_*`, `zeros_*`, `ones_*`,
  `identity_matrix`, `csr_extract_v`/`csr_extract_u`, and generalized `rep_array`.
  Support parameter matrices in `gp_exp_quad_cov` and `normal_id_glm_lpdf`,
  `is_inf`, additional constants, and transparent `profile(...)` wrappers.
- Extend generated-quantities density support to negative-binomial-2 forms and
  `multi_normal_lpdf`. Add `gumbel_rng`, `dirichlet_rng`, and `beta_binomial_rng`,
  including runtime-control support alongside `exponential_rng`.
- Show NUTS diagnostics in the browser: divergences, treedepth, E-BFMI, R-hat,
  and bulk/tail ESS. Mark unavailable or incomplete diagnostics, preserve draws
  if reporting fails, and show generated quantities in a collapsed table.
  (#292)

### Fixes

- Use Stan Math's `multiply_lower_tri_self_transpose` implementation, correcting
  results for matrices with nonzero upper triangles.
- Restore the Windows export of `stanli_wa_n_generated_start`, fixing linked
  clients and generated-quantity export.
- Keep the selected parameter plot visible during keyboard navigation.

## 0.10.0

- Expand ctsem support: `crossprod`, `tcrossprod`, `add_diag`, `matrix_exp`,
  `mdivide_left`, `mdivide_right_spd`, `quad_form_sym`, and parameter-sensitive
  `while` loops with loop-carried integers.
- Preserve value-dependent extents and logical shapes of integer-array user
  function results, including empty arrays, conditional returns, and indexed
  writes. Add multidimensional scatter assignment with last-write-wins repeats.
- Evaluate `multi_normal_cholesky_lpdf` in the output interpreter, including
  arrays of vector observations.
- Collapse loops that only add iterator-independent target terms to one
  evaluation multiplied by the trip count. (#248)
- Integrate eligible `ode_rk45`/`ode_ckrk` sensitivities directly from compiled
  callbacks, avoiding nested autodiff. Unsupported callbacks retain Stan Math's
  path; `STANLI_NO_ODE_DIRECT_RK=1` selects it explicitly.
- Treat infinite declaration bounds as identity transforms, including mixed
  finite/infinite vector bounds. (#250)
- Support full-span reads and mixed two-axis matrix selections. Improve
  compile-time evaluation of `ceil`, real comparisons, self-referential integer
  assignments, and locally built index arrays. (#247)

## 0.9.6

- Add CmdStan-style sampling progress, timings, divergence warnings, and
  treedepth warnings in Python and R. `refresh` controls updates; `refresh=0`
  keeps sampling quiet. Reporting runs on the calling thread without changing
  draws or RNG streams. (#230)
- Preserve shape and integer representation in whole-value indexed assignments
  such as `x[:] = rhs` across all execution paths.

## 0.9.5

- Enable upstream O1 plus `vectorize_loops` in every portable compiler. Keep
  the pass-off pipeline as a test oracle and retain C++ loop re-rolling.

## 0.9.4

- Switch R and webR to the shared, exact-source JavaScript compiler and compact
  Portable MIR v2. Compiler errors are final, without retrying the legacy export.
- Record compiler provenance and byte-check the artifact against a fresh build.
  Verify compatibility between current packages/runtimes and v0.9.3 in both
  directions, covering native, V8, and webR compilation.

## 0.9.3

- Share one OCaml compiler pipeline across native, browser, npm, and Windows,
  with byte-identical canonical output. Retain legacy compilers for rollback,
  while reporting selected-compiler errors directly. (#211, #213)
- Introduce compact Portable MIR v2 with bounded allocation and a retained
  legacy reader. Fixture payload falls from 4.65 MB to 0.82 MB; Eight Schools
  decode time falls from 0.293 ms to 0.074 ms. (#218)
- Prepare R/webR helpers for the shared compiler while retaining the legacy
  compiler in this release, providing a compatibility bridge for v0.9.4. (#225)
- Add independent upstream pass selection and a test-only `vectorize_loops`
  probe; production vectorization remains off in this release. (#215, #217)
- Expand parameter-dependent control flow to empty outputs, shape queries,
  integer operations and arrays, logical operators, `rep_vector`, `break`,
  `continue`, uninitialized locals, and zero-width inputs. (#223)
- Key the R runtime cache by pinned release, prune obsolete runtimes on install,
  and remove `stanli_install(version = "latest")`. Custom runtimes require
  `STANLI_RUNTIME`. Reported by @StaffanBetner. (#220)

## 0.9.2

- Ship the missing `stanli_wa_seed_chain` C symbol and update R's runtime pin,
  fixing R-universe package loading. Reported by @StaffanBetner; install docs
  now lead with R-universe. (#207, #204)
- Expand language support for `tcrossprod`, `rep_row_vector`, data-bounded
  `while`, conditionally unwritten locals, row ranges, index-vector/pair writes,
  and integer-local size expressions. Includes conditional container sizes and
  transformed-data `diag_matrix`. Contributions from @andrjohns.
  (#205, #185, #186)
- Compact island register programs before generating adjoints and skip partials
  for data-only density arguments.
- Fix the legacy Powell `algebra_solver` callback lifetime through the complete
  Jacobian sweep. (#209)
- Build stanc3 from a pinned source SHA instead of mutable nightly assets. (#208)

## 0.9.1

### Performance

- Fuse nonadjacent observation lanes into vector operations and synthesize IRT
  GLMs, improving affected models by up to 6.5x. `STANLI_NO_PARTITION=1`
  disables the pass. (#190, #191, #192)
- Eliminate repeated target terms with mutation-aware common-subexpression
  elimination: `Mt_model` improves 16.1x. `STANLI_NO_CSE=1` disables it. (#184)
- Use closed-form mixture partials, reuse partials in eligible multivariate
  densities and GLMs, add elementwise binomial kernels, and widen `pow` during
  re-rolling. Affected mixture models improve 26–47%; dogs models 56–69%.
  (#184, #189, #192)
- Streamline ODE Jacobian extraction and compact scalar callbacks:
  `lotka_volterra` falls from 78.5 to 60.9 µs per gradient. (#188)
- Eliminate redundant full-extent copies, improving `normal_mixture_k` by
  7.5–8%. (#195)
- Corpus median gradient speedup over CmdStan rises from 2.17x to 2.91x, with
  116 of 119 measured models at parity or faster. (#187, #193)

### Fixes and compatibility

- Prevent fusion from collapsing or deduplicating `print` and `reject`. (#181)
- Leave unwritten real generated-quantity container elements as NaN. (#182)
- Switch generated quantities to Stan's RNG, changing fixed-seed sequences.
  Caller streams use `create_rng(seed, 0)`; sampling frontends use independent
  per-chain streams. These output streams do not reproduce CmdStan's advanced
  sampler RNG state; the per-chain C symbol ships in 0.9.2.
- Add the stanc3 mother model to the oracle, with legacy Powell `algebra_solver`,
  zero-job `map_rect`, and mixed multidimensional reads and partial writes.
- Fusion can change gradients' last bits. All changes pass the CmdStan reference
  corpus; worst verified relative deviation remains 2.6e-12. Disable partition
  and CSE with the flags above to investigate differences.

## 0.8.5

- Bundle the matching runtime in wasm R packages, allowing webR to load it
  without downloading GitHub assets. `stanli_install()` recognizes the bundle.
  Suggested by @StaffanBetner. (#163)
- Compile scalar RNGs, vector reductions, and min/max in generated quantities,
  reducing `Mh_model` output time from 869 to 12.5 µs per row.
  (#170, #172, #173, #174, #175)
- Pack island adjoints densely, use a direct scalar categorical pullback, and
  seed mixed ODE inputs directly into registers. (#168, #169, #171)

## 0.8.4

- Reuse symmetric eigendecompositions, preserve ODE input activity types, and
  add a native single-observation multivariate-normal Cholesky pullback.
  `kronecker_gp` falls from 289.0 to 185.7 µs per gradient; `gp_regr` from
  6.05 to 4.20 µs, with bitwise value/gradient parity. (#160, #161, #162)
- Publish a webR runtime side module, pinned to webR 0.6.0 / Emscripten 4.0.8.
  Side modules require a compatible Emscripten version. (#163)
- Compile Stan source inside webR through its JavaScript engine, without V8;
  transfer source and MIR through the shared filesystem.

## 0.8.3

- Fix `target += container` to add all elements and their gradients, including
  in parameter-dependent control flow.
- Fix interpreter row-vector dimensions to return `rows=1`, `cols=n`.
- Match Stan Math's zero-boundary guards for `sqrt` and `pow`, preventing NaN
  gradients at inputs where CmdStan is finite.
- Replay all corpus models at three deterministic points with full references.
  Add coverage baselines, a stanc3 model census, bitwise cross-path comparisons,
  and AddressSanitizer checks on pull requests. Crashes block verification;
  the documented output-reduction disagreement near zero has a 1e-15 absolute
  tolerance.

## 0.8.2

- Initialize adopted registers to NaN in island and ODE programs, fixing crashes
  and wrong gradients when inlined functions appear in untaken branches.
  Conformance workers that crash now fail verification.
- Lower matrix division to linear solves, supporting matrix/matrix,
  row-vector/matrix, and matrix left-division by vectors or matrices, including
  generated quantities. Scalar and elementwise division retain their semantics.
- Reject out-of-bounds graph indices at compile time with named index/extent
  errors; improve interpreter indexing errors too.
- Support empty gathers, typed empty data arrays, and empty ranges. `hi < lo`
  produces an empty slice regardless of endpoints; only nonempty ranges check
  bounds. (#133)
- Reject incorrectly typed declared-integer data at binding time, naming the
  variable; JSON `1.0` is not accepted as an integer.

## 0.8.1

- Resolve overloaded user functions by argument type. (#125)
- Preserve full shapes of deeply nested array literals, fixing transposed data
  and incorrect gradients. (#122)
- Restore bitwise CmdStan parity for vector `inv_logit` and fix out-of-bounds
  GLM reads with scalar outcomes. (#123)
- Add named binary-operator aliases across execution paths, preserve integer
  division during constant folding, and support `von_mises_{cdf,lcdf,lccdf}`
  and `neg_binomial_2_{lcdf,lccdf}`. Verify 320 more conformance rows. (#124)

## 0.8.0

- Add single-path Pathfinder using Stan's service, with draws, the L-BFGS path,
  ELBO-selected iterate, and Pareto k-hat. It shares the initialization stream
  with NUTS and WALNUTS. Multi-path requires real TBB and is not supported.
- Add browser NUTS/Pathfinder comparisons with live optimization progress,
  shared histograms, Q-Q plots, and per-parameter discrepancies. Standalone
  Pathfinder shows optimization paths and histograms; k-hat measures weight
  stability and does not establish posterior coverage.

## 0.7.2

- Initialize WALNUTS at the same first finite point as NUTS for matched seed and
  chain ID, replacing 0.7.1's best-of-16 selection. Keep Stan's initial step-size
  search. WALNUTS sensitivity to deep-tail starts remains an upstream issue.

## 0.7.1

- Reduce WALNUTS freezes on stiff posteriors by selecting the highest-density
  initial point among 16 finite candidates and using Stan's
  `find_reasonable_epsilon` step-size search. The selection policy is replaced
  in 0.7.2; sampler algorithms are unchanged.

## 0.7.0

- Add `DataMap::from_var_context` for bindings with Stan-compatible data
  contexts, avoiding JSON serialization while preserving integer types and
  column-major layout. Its implementation can be built independently of the
  JSON reader. Requested by @andrjohns for `stanr`. (#81)

## 0.6.1

### Features

- Implement the BridgeStan C ABI over the shared runtime.
  `stanli.bridgestan_model(stan_file=..., data=...)` returns a BridgeStan model
  with MIR embedded under the reserved `__stanli` data key. Clients must permit
  modifying the data argument; library-path-only clients are unsupported.
  `tools/bs_conformance.py` checks against BridgeStan. See the
  [facade design](docs/superpowers/specs/2026-08-10-bridgestan-facade-design.md)
  and [embedded-MIR design](docs/superpowers/specs/2026-08-11-embedded-mir-data.md).
- Add WALNUTS through vendored walnutpie headers, with `run_walnuts`,
  `stanli_sample_walnuts_stream`, and npm's `sampler: "walnuts"`. Its
  `max_error` option controls within-trajectory step adaptation.
- Add browser NUTS/WALNUTS comparisons with synchronized parameter selection,
  shared plot axes, timing, ESS, and ESS/second. Bound live plotting work for
  large runs.
- Make generated-quantity RNG streams caller-owned internally; the existing C
  ABI keeps one stream per model.
- Route model `print()` through a serialized, replaceable
  `stanli::set_message_sink`, defaulting to stdout.
- Add `stanli_stan_to_mir` and Python's `stanli.stan_to_mir` for caching compiled
  MIR, plus `stanli_build_id` for identifying compatible runtime artifacts.

### Fixes

- Convert non-finite WALNUTS densities to `-inf` with zero gradients, preventing
  NaNs from poisoning warmup adaptation.
- Use a scratch RNG during column discovery and a fixed initial model seed,
  making optimization's generated quantities reproducible.

## 0.6.0

- Generate island reverse programs over doubles instead of replaying nested
  autodiff. Measured improvements reach 4.74x on `iohmm_reg`;
  `STANLI_NO_NATIVE_ADJ=1` restores replay. A `CALL` instruction lets island
  programs use the graph's full kernel vocabulary.
- Add ten executable stanc3 language fixtures alongside posteriordb, bringing
  the corpus to 129 models. Record deterministic output rows for every model,
  including parameter-only rows, and refresh references at in-support points.
- Fix transposed CSV output for arrays of matrices, normalization flags on user
  density calls, and the free-parameter count and transform of
  `sum_to_zero_matrix`.
- Rewrite [the contributor guide](docs/hacking.md), add a
  [lowering walkthrough](docs/lowering-walkthrough.md) and runtime-directory
  READMEs, and gate `clang-format` in CI.

## 0.5.1

- Re-release 0.5.0 with downloadable R runtime assets; no code changes. Publish
  releases only after attaching assets to the draft, fixing empty immutable
  releases.

## 0.5.0

Includes the shape fixes drafted for 0.4.1, which was never released.

### Sampling and bindings

- Run four chains in parallel by default, with independent RNG streams and
  draws identical to sequential execution. Enable `STAN_THREADS` in native
  builds and initialize each worker's autodiff stack.
- Add `fit.summary()` and `fit.diagnose()` with MCSE, quantiles, bulk/tail ESS,
  rank-normalized split-R-hat, divergences, treedepth, and E-BFMI.
  `fit.draws()` preserves chains; `fit[]` concatenates them; `fit.to_arviz()`
  includes sampler statistics.
- Add controls for chains, parallelism, thinning, saved warmup, unconstrained
  initial values, initialization radius, and tree depth in Python, C, and the
  CLI. Add `stanli_sample_multi`, `stanli_summary_stats`, `stanli_diagnose_text`,
  and `stanli_thread_safe` to the C API.
- Add L-BFGS optimization through `Model.optimize()`, returning output columns
  and an unconstrained point usable for initialization. It includes the
  transform Jacobian; `jacobian=False` is rejected. Constrained initialization
  and Pathfinder are not supported in this release.
- Add the R package: `stanli_model()`, `sample_model()`, `summary()`,
  `stanli_diagnose()`, `optimize_model()`, and `as_draws_array()`. Compile via
  embedded stanc3 or JavaScript/V8; `stanli_install()` downloads a pinned runtime
  and checks ABI compatibility. Available from
  [R-universe](https://seantalts.r-universe.dev) or a checkout; not yet on CRAN.
  (#33, #35)
- Put 117 verified models on the searchable, lazily loaded
  [demo page](https://seantalts.github.io/stanli/). Publish npm under
  `@seantalts/stanli`.

### Language support and fixes

- Add modern `ode_rk45`, `ode_bdf`, `ode_adams`, `ode_ckrk`, and `_tol`
  interfaces with vector states and variadic typed arguments, retaining the
  interpreter fallback. Fix invalid iterator ranges when packing ODE data.
- Add scalar/per-element `offset` and `multiplier`, including parameter-valued
  forms, plus `unit_vector`, `sum_to_zero_vector`, `corr_matrix`, `cov_matrix`,
  and square/rectangular `cholesky_factor_cov` transforms.
- Add transformed-data and model-block `print`/`reject`; parameter-dependent
  rejection remains unsupported in this release.
- Preserve per-evaluation initialization of buffers modified in place, fixing
  log densities that drifted between repeated evaluations. Resolve `rows()` in
  real arithmetic. Eight brms-shaped models verify against CmdStan. (#32)
- Fix data layout for whole arrays of vector outcomes in `multi_normal`,
  `multi_normal_cholesky`, `multi_normal_prec`, `multi_student_t`, and
  `multi_student_t_cholesky`.
- Support vectorized Dirichlet densities over arrays of simplexes and vectorized
  truncation, including one-sided bounds and count densities.

### Performance and maintenance

- Make re-rolling O(n log n) by replacing whole-use-list scans with bounded
  searches. Gate complexity with operation counts instead of wall time. (#36)
- Simplify runtime code and shorten documentation without measured behavior or
  performance changes; add coverage for optimization kill switches and
  `forward_value_only`. (#37, #39)

## 0.4.0

### Coverage and correctness

- Reach 71 of 72 densities, including count GLMs, ordinal, multivariate,
  multinomial, wishart, LKJ, and Wiener families, plus 90 of 105 distribution
  functions. `gaussian_dlm_obs` remains unsupported because it needs seven op
  inputs. See [coverage](docs/coverage.md).
- Support truncation/censoring through distribution functions and `log_diff_exp`.
- Honor GLM proportional-density flags, fixing constant offsets in `lp__`.
  Reject operations exceeding input capacity instead of corrupting memory.
- Use compact instantiations for the multivariate/multinomial tail: gradients
  and output values match CmdStan, but `lp__` can differ by a model-specific
  constant. See [compact densities](docs/compact-densities.md).
- Disable `STANLI_LITE_LP` by default everywhere, including the browser, so builds
  use the same `lp__` convention; compact-density limitations still apply.

### Browser, build, and tools

- Enable SIMD128, improving measured browser performance by 2–11% with bitwise
  gradient parity. Runtime payload is 1.52 MB gzipped; native library is
  22.2 MB installed and wheel 7.8 MB. The
  [density-pack experiment](docs/density-pack.md) records why side-loading
  uncommon densities was not retained.
- Split density kernels across nine translation units to reduce compiler memory
  and build serialization.
- Build `stanli_run` with in-process compilation when embedded stanc3 is
  available; install the CLI, checker, library, and headers via CMake.
- Report non-finite values in `stanli_check`. Add browser gradient benchmarks,
  lite-build verification, and `tools/verify_refs.py --no-lp` for builds with
  intentionally shifted log densities.
- Cancel superseded pull-request CI runs while preserving main and tag runs.

## 0.3.0

Includes changes drafted for 0.2.1, which was never released.

### Compatibility

- Switch sampling from `ecuyer1988` to CmdStan's `mixmax` initialization and RNG
  stream. Fixed seeds produce different draws than 0.2.0.
- Enable `STANLI_LITE_LP` by default in the browser, dropping density terms
  constant in active arguments. Gradients and outputs are bitwise identical to
  the exact build, but `lp__` shifts by a model-specific constant and rounding
  can change fixed-seed chains. Browser `lp__` is unsuitable for absolute-density
  comparisons, Bayes factors, marginal likelihoods, or bridge sampling.
  PyPI wheels retain exact builds. C's `stanli_exact_lp()`, Python's
  `stanli.exact_lp()`, and JS's `fit.exactLp` report the mode. See
  [lite log densities](docs/lite-lp.md). The browser default changes in 0.4.0.

### Browser and platform support

- Compile and sample Stan entirely in the browser using JavaScript stanc3 and
  WebAssembly. The [demo](https://seantalts.github.io/stanli/) provides parallel
  chains, live plots, R-hat/ESS, and CSV downloads.
- Add npm package `@seantalts/stanli` with `compile()`, `sample()`, streaming
  `onLive`, and `preload()`. Gzipped payload: 0.99 MB runtime plus 0.43 MB
  compiler; precompiled MIR avoids the compiler download.
- Replay 118 corpus models through WASM. `nn_rbm1bJ100` exceeds wasm32's 4 GB
  address space.
- Add Windows x86_64 wheels built with mingw-w64 and a bundled subprocess
  `stanc.exe`; MSVC and embedded Windows compilation are unsupported.

### Language and sampling

- Reach 46 of 72 densities, 87 of 105 distribution functions, and 47 of 129
  scalar functions, including truncation/censoring support. Add 34 scalar math
  functions and 18 distributions, verified against stanc3 signatures and
  CmdStan. See [coverage](docs/coverage.md).
- Offer `-DSTANLI_LITE_LP=ON`, reducing the stripped runtime from 14.9 MB to
  7.79 MB with the compatibility tradeoffs above.
- Check both value-only density and gradients during initialization, matching
  CmdStan's acceptance policy and fixing `lotka_volterra` sampling timeouts.
- Write NaN outputs and continue when generated quantities throw, preserving
  the rest of the sampled draws.
- Expose output rows through `stanli_wa_n_columns`, `stanli_wa_column_name`,
  `stanli_wa_seed`, and `stanli_wa_row`.
- Support a self-contained `stanli_run` with embedded stanc3: Stan source and
  JSON data in, CmdStan-shaped CSV out, without a separate toolchain.

### Performance and tooling

- Reuse `normal_id_glm_lpdf` partials and improve matrix-vector accumulation,
  with bitwise parity. Sampling time falls from 108 s to 58 s for diamonds and
  175 s to 98 s for prophet; median corpus gradient speedup reaches 2.07x CmdStan.
- Fix PyPI benchmark-table rendering and verify it with `readme_renderer` in
  `tools/gen_docs.py --check`.
- Add lite/exact comparison tooling and signature-derived function sweeps.

## 0.2.0

- Produce transformed parameters and generated quantities for all 119 compiling
  corpus models, using a per-draw interpreter where the output graph cannot
  express RNGs or runtime-dependent shapes/control flow.
- Compile parameter-dependent branches and ternaries to register programs with
  autodiff replay of the selected arm.
- Replay CmdStan density/gradient references in CI on four platforms, adding
  deterministic output checks for 20 models. Fix uninitialized real values to
  NaN and transposed batched-simplex output.
- Resolve kernel dispatch at bind time, unroll executor sweeps, fuse mixture
  lanes and stores, and fix re-rolling's compile-time blowup on repeated vector
  refills. Median gradient speedup reaches 2.00x CmdStan, with 92 of 119 models
  at parity or faster.
- Reject and retry non-finite initialization draws.
- Share the MIR interpreter across transformed data, ODEs, and generated
  quantities, and unify ODE/island register machines.
- Make Python's `Model.log_prob_grad` raise on failed evaluations and reject
  wrong-sized points with `ValueError`.
- Add `STANLI_PROFILE=1`, sampler differential tracing, a
  [contributor guide](docs/hacking.md), and generated documentation metrics.

Python `sample()` still returns declared parameters only; transformed parameters
and generated quantities are available through CLI CSV. Variational inference,
optimization, multi-chain threading, and Windows wheels are not yet supported.

## 0.1.0

First public release.

- Compile and sample Stan without a C++ toolchain using embedded stanc3 and an
  operation graph over precompiled Stan Math kernels.
- Run NUTS with diagonal-metric adaptation and maximum tree depth 10.
- Verify 118 of 120 posteriordb models against CmdStan: 45 bitwise identical,
  worst relative deviation 2.6e-12. Nine of ten benchmark models run gradients
  1.1–6.2x faster; `low_dim_gauss_mix` runs at 0.53x. Time to first draw is
  roughly 20x faster.
- Optimize graphs with loop re-rolling, in-place updates, forwarding,
  dead-write removal, and constant folding. `STANLI_NO_REROLL=1` disables
  re-rolling.
- Compile ODE callbacks to a register machine and solve values/sensitivities
  together, improving ODE evaluation by 29–39x. `STANLI_DEBUG_ODE=1` reports
  interpreter fallbacks.
- Write transformed parameters and generated quantities through a forward-only
  graph for 93 of 119 compiling models in the CLI.
- Ship macOS arm64/x86_64 and Linux x86_64/aarch64 wheels, 13.8 MB installed.

Python sampling returns declared parameters only. Variational inference,
optimization, multi-chain threading, convergence diagnostics, and Windows wheels
are not yet supported.
