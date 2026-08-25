# Benchmarks vs CmdStan

2026-08-25, Apple M3 Ultra (macOS arm64), Apple clang 21, both sides `-O3`
and `-ffp-contract=off`, single-threaded. The stanli columns were refreshed as
one 120-model run after the optimization stack landed. The CmdStan columns are
unaffected by those stanli changes and carry over from the 2026-08-06 run on
the same host. Both engines evaluate the sampling gradient (propto + jacobian)
at the same deterministic unconstrained point. stanli runs
`tools/bench_grad.cpp`; CmdStan runs `tools/bench_cmdstan_grad.cpp`, looping the
same fresh-vars + grad + recover_memory cycle
`stan::model::gradient` performs per leapfrog step. Reproduce the stanli side
by passing `--stanli-only` to `harnesses/corpus_bench.py`. To remeasure both
sides, omit the flag and write to a new output path; an existing TSV is treated
as resumable completed work.

The gradient cells in this corpus snapshot are warmed arithmetic means from
one timed loop per model. Preparation is measured separately with
`bench_grad --prep`: optimized MIR and JSON read/parse, graph compile, executor
construction, and binding, with no stanc time, gradient warmup, or evaluation.
Optimization-specific sections later on this page preserve targeted
before/after medians from their original A/B runs. Those historical medians
explain attribution; the current corpus rows below are the source of truth for
absolute performance.

## Per-gradient latency

A representative slice; the complete 120-model table is at the bottom of
this page.

| model | unconstrained params | stanli ns/grad | CmdStan ns/grad | speedup | stanli prep |
| --- | ---: | ---: | ---: | ---: | ---: |
| `radon_pooled` | 3 | 50,669 | 320,938 | 6.33x | 0.018 s |
| `arK` | 7 | 1,754 | 12,459 | 7.10x | 0.002 s |
| `radon_hierarchical_intercept_centered` | 391 | 97,029 | 569,143 | 5.87x | 0.042 s |
| `radon_county_intercept` | 388 | 81,826 | 431,614 | 5.27x | 0.027 s |
| `nes` | 10 | 16,046 | 69,324 | 4.32x | 0.004 s |
| `eight_schools_noncentered` | 10 | 302 | 745 | 2.47x | 0.001 s |
| `election88_full` | 90 | 292,172 | 901,961 | 3.09x | 0.153 s |
| `bym2_offset_only` | 3845 | 40,559 | 114,620 | 2.83x | 0.002 s |
| `dogs` | 3 | 9,098 | 63,747 | 7.01x | 0.012 s |
| `kidscore_momiq` | 3 | 1,528 | 4,861 | 3.18x | 0.001 s |
| `lsat_model` | 1006 | 43,739 | 91,173 | 2.08x | 0.003 s |
| `state_space_stochastic_level_stochastic_seasonal` | 389 | 18,718 | 26,320 | 1.41x | 0.002 s |
| `hmm_example` | 4 | 20,557 | 27,145 | 1.32x | 0.008 s |
| `garch11` | 4 | 7,034 | 9,664 | 1.37x | 0.002 s |
| `hmm_drive_0` | 6 | 120,467 | 132,850 | 1.10x | 0.037 s |
| `normal_mixture` | 3 | 85,679 | 88,239 | 1.03x | 0.003 s |
| `low_dim_gauss_mix` | 5 | 90,323 | 98,315 | 1.09x | 0.003 s |
| `wells_dist100ars_model` | 3 | 17,446 | 18,997 | 1.09x | 0.002 s |
| `iohmm_reg` | 29 | 243,847 | 320,335 | 1.31x | 0.270 s |
| `radon_county` | 389 | 73,180 | 82,076 | 1.12x | 0.011 s |
| `arma11` | 4 | 4,550 | 6,158 | 1.35x | 0.002 s |
| `diamonds` | 26 | 31,202 | 31,497 | 1.01x | 0.021 s |
| `ldaK2` | 7 | 95,716 | 104,059 | 1.09x | 0.006 s |

Generated adjoints and native runtime control put every sequential model in
this slice at parity or better: `arma11` and `garch11` are 1.35-1.37x, the
fixed-state HMM rows are 1.10-1.32x, and `iohmm_reg` is now 1.31x. The island
section below preserves the targeted per-region A/B that isolates what
generated adjoints bought independently of absolute run noise.

## Which models are faster, which are slower, and why

Across the full corpus (`docs/corpus-bench.tsv`, 119 models with both
gradients) the median per-gradient speedup is 2.17x and 104 of 119
models are at or above parity. The ratio is predicted almost entirely
by the model's *shape*, not its size.

**Faster (most of the corpus, typically 1.5-8x):**

- **Vectorized-statement models.** A `y ~ normal(X * beta, sigma)` over
  N observations is a handful of ops here; CmdStan builds and walks N
  var-tape nodes per statement per leapfrog step. The gap grows with N.
  This class is regressions, GLMs, and most hierarchical models.
- **Scalar loops the passes can vectorize.** The hierarchical indexing
  idiom (`y[n] ~ normal(mu[county[n]], sigma)` and loops that fill a
  vector element by element) arrives unrolled and is re-rolled back
  into the class above: the radon family up to 6.3x, `election88_full`
  3.1x, `dogs` 7.0x.
- **Nested fixed-width mixtures.** LDA's per-document `gamma[K]` construction
  and row `log_sum_exp` become two packed gathers, vector arithmetic, and one
  row-reduction op. In the current corpus run, `ldaK2` is 95.7 us/gradient
  (1.09x CmdStan) and `ldaK5` is 3.74 ms/gradient (1.49x).
- **Everything, on preparation.** The MIR/data-to-bound-executor path has a
  2 ms median; 76 of 119 measured models prepare in at most 5 ms. The largest
  JSON input, `nn_rbm1bJ100`, takes 3.158 s and `ldaK5` takes 0.293 s, against
  carried-over CmdStan builds of 5.2 s and 3.5 s respectively.

**Near parity (roughly 0.9-1.5x), three shapes:**

- **Models dominated by one large dense operation** (a Cholesky, a
  big matrix product, the GP models). Both engines spend their time
  inside the same stan-math kernel; interpreter overhead is noise on
  top.
- **Sequential models.** HMM recursions and state-space/ARMA/GARCH
  updates read the previous step's parameter-dependent result, so
  re-rolling correctly refuses (vectorizing a recurrence would change
  the math). This is the class the island pass is for, and it sat at
  0.6-0.9x until the island backward stopped being a var replay and
  became a generated adjoint program: `hmm_example` is now 1.32x,
  `garch11` 1.37x, `hmm_gaussian` 1.19x, and the two `hmm_drive` models
  1.10-1.17x against CmdStan. What keeps them at parity-plus rather than
  higher is per-op dispatch against CmdStan's inlined scalar code, one
  interpreted instruction at a time in each direction (the island section
  below has the targeted per-region numbers). Native runtime control now puts
  `iohmm_reg` at 1.31x as well.
- **Matrix-filling updates.** `Mtbh_model` re-rolls 584 element writes into
  146 strided stores. A second in-place pass now keeps one initial copy and
  makes all 146 stores destructive, reducing their profile share from 42.4%
  to 5.5%. Native Bernoulli forwards then remove recorder overhead; the
  current `Mt_model`, `Mth_model`, and `Mtbh_model` rows are 1.06x, 1.65x,
  and 1.59x CmdStan.

**Slower (a shrinking tail, mostly 0.5-0.9x):**

- **ODE models**: `lotka_volterra`, `soil_incubation`, and
  `one_comp_mm_elim_abs` are 0.59x, 0.59x, and 0.76x.
  The remaining gap is our per-call dispatch of the compiled right-hand side
  against CmdStan's native C++ right-hand side inside the same underlying Stan
  Math integrator. (They were 0.015-0.025x before the right-hand side compiled;
  see below.)
- **Occupancy and one matrix-filling shape**: `multi_occupancy` and
  `dogs_nonhierarchical` are both 0.78x. These are the only non-ODE rows below
  0.8x in this snapshot; `iohmm_reg`, GPCM, and GRSM have moved to 1.31x,
  1.32x, and 1.50x respectively.

A profile of every sub-parity model (`STANLI_PROFILE=1`) says the
remaining tail is mostly not a graph problem: in seven of those models
a single precompiled kernel is half to nine-tenths of the gradient.
`diamonds` was the extreme, 90.9% in one GLM kernel that rebuilt a var
tape in both sweeps; differentiating once and stashing the partials took it
0.48x -> 0.89x in that targeted A/B; the current warmed mean is 1.01x.
`prophet` was 82% in `OP_MATVEC` with one serial accumulation chain; four
independent accumulators took it 0.67x -> 1.23x in the targeted A/B and it is
1.23x in the current corpus run, bitwise unchanged.

## Where the wins come from

The interpreter's cost is per op, not per element: ~17-20 ns for a
scalar density op forward + backward, against ~0.9 ns for the actual
math (`tools/bench_opcost.cpp`). A vectorized statement over N elements
amortizes that to nothing and runs precompiled stan-math on contiguous
doubles, while CmdStan pays its var-tape cost per scalar per
evaluation. Vectorized models therefore win, and models stuck as
unrolled scalar loops used to lose (0.4-0.9x) until the graph passes
closed the gap. The passes themselves are described in
[runtime/src/OPTIMIZATIONS.md](../runtime/src/OPTIMIZATIONS.md); the
headline measurements follow. These are the historical targeted medians described at
the top of the page, not replacements for the current corpus rows:

- **Re-rolling** (scalar loops back to vector ops): `radon_pooled`
  27,670 ops -> 8 (0.91x -> 6.18x), `arK` 3,164 -> 21 (0.40x -> 4.83x).
- **In-place updates + store-to-load forwarding** (the element-write
  idiom): `radon_county_intercept` went from 90.5 ms per gradient in
  2.58 GB of arena (207x slower than CmdStan) to 92 us in 42 MB, 77,960
  ops -> 9. Seven radon-family models and `rats_model` collapse the
  same way.
- **Write-side fusion** (loops that fill a vector something else
  reads): `radon_county` 25,152 ops -> 10 (0.36x -> 0.98x),
  `election88_full` 289,165 -> 65 (0.39x -> 2.97x). Strided-run and
  integer-outcome fusion closed the `dogs` family (0.65x -> 2.8x).
  57 of the 120 corpus models change under the passes, against 28
  before write-side fusion.
- **Post-reroll in-place slices** (chained partial matrix fills):
  `Mtbh_model` keeps the same 1,585-op graph, but 146 stores now move four
  values rather than copying a 730-value matrix. Median gradient latency
  falls 106.5 -> 47.4 us (2.24x); direct replay remains within 3.61e-15 of
  the CmdStan references across 465 values.
- **Native Bernoulli forwards** (recorder-bound scalar and short-vector
  calls): both Bernoulli parameterizations have one real argument and one
  analytic partial, so they write that column directly into the density
  scratch instead of constructing the generic recorder edge. Summed vector
  logits preserve Eigen's packet `exp`, select, and reduction order while
  reusing the partial column as their `ntheta` workspace. `Mt_model` falls
  30.6 -> 19.2 us, `Mth_model` 113.2 -> 57.3 us, and `Mtbh_model` 47.4 ->
  26.8 us after the slice fix. The combined `Mtbh_model` improvement is
  106.5 -> 26.8 us (3.98x). In the full warmed-mean snapshot the three models
  are 19.0, 57.1, and 27.0 us/gradient, or 1.05x, 1.65x, and 1.59x CmdStan.
- **Native scalar probability categorical** removes the nested autodiff replay
  only when one categorical outcome selects from an active probability vector.
  Stan Math's double overload still computes the value and performs every
  check; reverse adds the incoming seed divided by the selected probability to
  that probability's adjoint. Array outcomes retain replay to preserve their
  repeated-selection accumulation topology, and categorical-logit calls retain
  replay for their dense pullback. The graph is unchanged. In a targeted
  2026-08-24 Release A/B (seven matched-run medians),
  `gpcm_latent_reg_irt` moved 1.741465 -> 0.955609 ms/gradient (1.8224x
  internally, now 1.3998x CmdStan), and `grsm_latent_reg_irt` moved
  0.9705208 -> 0.4953192 ms/gradient (1.9594x internally, now 1.5387x
  CmdStan). Their categorical opcode time fell 4.45x and 5.00x respectively;
  the categorical-logit RBM controls were unchanged. These targeted medians
  are not replacements for the current full-corpus warmed means of 1.014034
  ms (1.32x CmdStan) and 0.508863 ms (1.50x CmdStan) in the table below.
- **Compiled scalar generated-quantities RNGs** keep caller-owned chain state
  on the forward-only write-array graph for scalar `poisson_log`, `uniform`,
  `bernoulli`, `normal`, `lognormal`, and `binomial` draws.
  Apart from the audited categorical and multivariate-normal extensions below,
  other RNGs, container-valued results, and draws used as dynamic control,
  indices, or geometry still fail closed to the whole-section interpreter. In
  an exact census of the 24 previously
  interpreted corpus models, this RNG tranche moved 12 to the graph; all 24
  still produced complete rows.
  A targeted 2026-08-24 C-ABI A/B (point 0, two warmups, seven matched batch
  medians) measured:

  | model | interpreted row | compiled row | improvement |
  | --- | ---: | ---: | ---: |
  | `covid19imperial_v2` | 156.239 ms | 2.089 ms | 74.81x |
  | `covid19imperial_v3` | 157.176 ms | 2.077 ms | 75.69x |
  | `dogs_hierarchical` | 1.929 ms | 24.105 us | 80.02x |
  | `dogs_nonhierarchical` | 2.078 ms | 32.317 us | 64.29x |
  | `GLMM1_model` | 75.541 us | 2.176 us | 34.72x |
  | `hierarchical_gp` | 2.291 ms | 46.301 us | 49.47x |
  | `lotka_volterra` | 1.797 ms | 31.434 us | 57.16x |
  | `one_comp_mm_elim_abs` | 5.123 ms | 139.431 us | 36.74x |
  | `M0_model` | 15.378 us | 0.119 us | 129.55x |
  | `Mb_model` | 2.020 ms | 26.703 us | 75.65x |
  | `Rate_4_model` | 4.007 us | 0.126 us | 31.93x |
  | `Rate_5_model` | 4.311 us | 0.124 us | 34.82x |

  The largest setup tradeoff is the two Covid graphs: C-API model
  construction rises from about 0.239 s to 2.20-2.21 s, but the 154 ms saved
  per row repays it after roughly 13 draws. For the four added scalar-binomial
  models, across 1,000 rows of each model their aggregate time falls from
  2.0438 s to 0.0271 s (75.50x), and construction also gets faster in every
  case, so break-even is immediate. Their graph and frozen-interpreter rows
  were bitwise identical for all 28,926 compared values. These are targeted
  write-array medians, not replacements for the sampling columns in the full
  corpus table.
- **Compiled generated-quantities reductions** add an exact forward-only
  product for the vector/row-vector surfaces used by the capture-recapture
  models, plus integer sum only when a one-dimensional runtime array is proved
  fully initialized, integral, and safe from 32-bit overflow. Product grouping
  follows Stan Math's expression provenance: materialized vectors use its
  address-independent packet grouping, while strided matrix-row expressions
  retain ascending scalar grouping. Shifted views, arbitrary expressions,
  reverse-mode products, and unproved integer arrays still fail closed to the
  interpreter. This moved `Mh_model`, `Mt_model`, `Mtbh_model`, and `Mth_model`
  to the graph, taking the then-current 24-model census from 12 graph / 12
  interpreter to 16 / 8. The categorical tranche below subsequently advances
  that census to 17 / 7, the extrema tranche advances it to 18 / 6, and the
  multivariate-normal tranche advanced it to 19 / 5, and the runtime-control
  tranche below completes it at 24 / 0. All 119 compiling corpus models still
  produce complete rows. A targeted 2026-08-24
  C-ABI A/B (point 0, two warmups, seven matched
  batch medians) measured:

  | model | interpreted row | compiled row | improvement |
  | --- | ---: | ---: | ---: |
  | `Mh_model` | 869.179 us | 12.494 us | 69.57x |
  | `Mt_model` | 20.867 us | 0.127 us | 164.83x |
  | `Mtbh_model` | 1.046 ms | 9.856 us | 106.18x |
  | `Mth_model` | 1.099 ms | 18.205 us | 60.36x |

  Across 1,000 rows of each model, aggregate row time fell from 3.035338 s to
  0.040682 s (74.61x). Including one construction of each model, it fell from
  3.056579 s to 0.075491 s (40.49x); the equal-mix aggregate setup cost breaks
  even after five rows per model. Within a 146,196-value comparison, every
  product-fed draw and final integer sum matched the frozen interpreter
  bitwise, including a fourth-row stream-continuation check. `Mtbh_model` and
  `Mth_model` also expose the pre-existing graph/interpreter boundary in
  deterministic transformed parameters: their `p` columns differ by at most
  two ULP, while the graph is closer to live CmdStan at the shared point. These
  are targeted write-array medians; the full-corpus sampling table awaits the
  next refresh.
- **Compiled categorical generated-quantities RNGs** extend the same
  scalar-result `OP_RNG` path to `categorical_rng(vector)`. The graph and
  `WaInterp` each copy their materialized probability vector into one shared
  helper and call the pinned Stan Math implementation, preserving its simplex
  validation order, one-based int result, and exact caller-owned stream
  consumption. Lowering marks the int initialized but deliberately does not
  infer a `[1, K]` range, so a later dynamic index still selects the
  whole-section interpreter.

  In a targeted matched C-ABI A/B, `Survey_model` moved from 1011.4736 to
  60.3127 us/row (16.7705x), saving 0.9511609 s per 1,000 rows. Construction
  moved from 4231.333 to 5406.709 us, so the 1175.376 us setup delta amortizes
  after 1.236 rows, or two whole rows. It was the only model to change in the
  exact 24-model write-array census, moving graph/interpreter coverage from
  16 / 8 to 17 / 7; all 24 census models and all 119 compiling corpus models
  still produced complete rows. At that stage, `iohmm_reg` remained
  interpreted at the later dynamic-index barrier, `value must be known at
  compile time: hatz`; the runtime-control tranche below now compiles that
  index and its subsequent Viterbi decoder.

  The categorical draw `n` matched exactly in all 18/18 C-ABI comparisons:
  seeds 0, 1, 2, 7, 1234, and `UINT32_MAX`, each continued for three sequential
  rows.
  The same full-row comparison recorded 8,532 expected bit differences in
  deterministic columns from the pre-existing graph/interpreter numerical
  boundary; they are not RNG mismatches. Focused tests avoid that boundary by
  routing the identical probability vector through the shared helper and the
  direct pinned Stan Math call, then comparing the next engine state. These
  targeted results do not refresh `docs/corpus-bench.tsv` or the generated
  full-corpus table below.
- **Compiled generated-quantities extrema** add the forward-only
  `OP_EXTREMA_VEC` min/max variants. Lowering admits only a top-level
  write-array call on a direct `UVector` or `URowVector` `Var` and evaluates
  it with the same address-independent grouping that pinned Stan Math uses for
  an owning Eigen value. Empty-real results are preserved: `min` is positive
  infinity and `max` is negative infinity. Arrays, matrices, indexed views,
  expressions, and UDF calls still select `WaInterp`; reverse-mode uses remain
  refused.
  The opcode is explicitly excluded from reroll matching and interpreter
  islands because it has no reverse kernel.

  `losscurve_sislob` was the only model to change in the exact 24-model
  census, moving graph/interpreter coverage from 17 / 7 to the then-current
  18 / 6; the multivariate-normal tranche below advanced it to 19 / 5, and the
  runtime-control tranche completes it at 24 / 0. All 24 census models and all
  119 compiling corpus models retained complete rows. Its 1,218-op graph
  contains exactly one length-10 min opcode
  and one length-10 max opcode, and writes 384 columns. Graph and `WaInterp` matched
  bitwise for all 1,536/1,536 compared values; the same-input pinned Stan Math
  check matched 8/8 cases.
  Against 1,200 stored CmdStan values, the worst difference was 4.44e-16, or
  eight ULP.

  In a targeted matched C-ABI A/B, `losscurve_sislob` moved from 329.9520 to
  3.3704 us/row (97.8970x), saving 0.3265816 s per 1,000 rows. Construction
  moved from 4107.375 to 5291.959 us, a 1184.584 us setup increase that
  amortizes after 3.627 rows, or four whole rows. These targeted results do not
  refresh `docs/corpus-bench.tsv` or the generated full-corpus table below.
- **Compiled covariance-form multivariate-normal RNGs** extend `OP_RNG` to
  the audited `multi_normal_rng(vector, matrix) -> vector` write-array surface.
  The mean length and square covariance shape are fixed by lowering, and both
  the graph and `WaInterp` copy their column-major inputs into the same owning
  Eigen values before calling pinned Stan Math. This preserves its finite,
  symmetry, and positive-definiteness validation order and its exact normal
  draw schedule. Array overloads, non-square or mismatched shapes, and
  `multi_normal_cholesky_rng` remain on the whole-section interpreter.

  `multi_occupancy` was the only model to change in the exact 24-model census,
  moving graph/interpreter coverage from 18 / 6 to the then-current 19 / 5.
  The runtime-control tranche below subsequently completes the census at
  24 / 0 and makes all 119 compiling corpus write arrays graph-backed. All
  rows remain complete. Its
  graph and forced-interpreter rows were bitwise identical for all 5,616
  values from six seeds continued for three sequential rows.

  In a targeted 2026-08-25 matched C-ABI A/B (point 0, two warmups, seven
  batch medians), `multi_occupancy` moved from 298.9260 to 5.4898 us/row
  (54.4512x), saving 0.2934362 s per 1,000 rows. Construction also improved,
  from 5864.792 to 5368.583 us, so there is no setup break-even penalty. These
  targeted results do not refresh `docs/corpus-bench.tsv` or the generated
  full-corpus table below.
- **Compiled generated-quantities runtime control** completes the last five
  interpreted write arrays: `hmm_drive_0`, `hmm_drive_1`, `hmm_example`,
  `hmm_gaussian`, and `iohmm_reg`. Their shapes and loop bounds are static, but
  their branches and Viterbi backtracking indices depend on the current draw.
  Lowering now places each enclosing block in one structured register program,
  with checked one-level dynamic indexing and packed live-ins when the block
  needs more than six logical inputs. The historical 24-model census moves
  from 19 graph / 5 interpreter to 24 / 0; all 119 compiling corpus models now
  have complete, graph-backed write arrays.

  A targeted 2026-08-25 matched C-ABI A/B (point 0, two warmups, seven batch
  medians) measured:

  | model | interpreted row | compiled row | improvement |
  | --- | ---: | ---: | ---: |
  | `hmm_drive_0` | 6.777 ms | 41.975 us | 161.45x |
  | `hmm_drive_1` | 7.396 ms | 44.426 us | 166.48x |
  | `hmm_example` | 1.388 ms | 9.718 us | 142.85x |
  | `hmm_gaussian` | 37.366 ms | 649.125 us | 57.56x |
  | `iohmm_reg` | 38.642 ms | 713.011 us | 54.20x |

  Across 1,000 rows of each model, aggregate row time fell from 91.568918 s
  to 1.458255 s (62.79x). Including one construction of each model, it fell
  from 91.989657 s to 1.990729 s (46.21x); the aggregate setup increase pays
  back after two whole rows per model. The four HMMs matched 51,696 checked
  Viterbi outputs bitwise across four points, three seeds, and three sequential
  rows. In `iohmm_reg`, all categorical and Viterbi states/scores matched; the
  only differences were 2,945 continuous simulations inheriting the
  pre-existing transformed-input boundary, bounded by 8.89e-16. Exact later
  state draws confirm stream alignment. These targeted results do not refresh
  `docs/corpus-bench.tsv` or the generated full-corpus table below.
- **Allocation-free ODE right-hand-side input seeding** removes the promoted
  `y` and `theta` staging vectors built on every solver callback and seeds the
  reusable register file directly. A targeted 2026-08-24 Release A/B (seven
  alternating matched-batch medians) measured:

  | model | staged inputs | direct register seeding | improvement |
  | --- | ---: | ---: | ---: |
  | `lotka_volterra` | 78.6216 us | 68.6068 us | 1.14597x |
  | `soil_incubation` | 102.1527 us | 89.5593 us | 1.14062x |
  | `one_comp_mm_elim_abs` | 643.1571 us | 578.8621 us | 1.11107x |

  The geometric-mean improvement is 1.13245x, and the known callback counts
  put the saving at a consistent 38.5-39.3 ns/callback. LP and gradient
  results were bitwise identical for all 63/63 checked scalars across three
  points. This targeted A/B attributes the direct-seeding change; the current
  corpus rows above and in the table below are the later end-to-end refresh.
- **Direct ODE Jacobian-row harvest plus scalar RHS cleanup.** Stan Math's ODE
  outputs are already precomputed-gradient nodes directly connected to active
  inputs, so the kernel now chains the selected output instead of running a
  nested reverse sweep across every sibling output for each Jacobian row, and
  clears only that output and its inputs between rows. A conservative
  load-time pass also removes overwritten scalar initializer
  constants and aliases stable single-definition copies; any range-reading,
  dynamic, density, or kernel instruction leaves the RHS program unchanged. A
  targeted 2026-08-25 three-binary A/B (seven rotating matched-batch medians)
  measured:

  | model | parent | direct node only | full change | improvement |
  | --- | ---: | ---: | ---: | ---: |
  | `lotka_volterra` | 78.5160 us | 70.3923 us | 60.8881 us | 1.28951x |
  | `soil_incubation` | 101.3979 us | 88.5437 us | 82.0553 us | 1.23573x |
  | `one_comp_mm_elim_abs` | 663.9302 us | 665.3790 us | 629.7752 us | 1.05423x |

  The full change improves the geometric mean by 1.18876x. Fresh parent and
  patched checkers were byte-identical for all 63/63 LP-and-gradient scalars
  over three evaluation points. These targeted medians isolate the mechanism;
  they do not refresh the full-corpus table or its retained CmdStan columns.
- **Packed row-wise reductions** (the LDA inner loop): targeted medians fall
  from 154 to 94 us for `ldaK2` and 6.82 to 3.70 ms for `ldaK5`, while their
  graphs collapse from 15,854 to 22 and 434,126 to 156 ops. The full
  warmed-mean rows are 95.7 us (1.09x) and 3.74 ms (1.49x).
- **Native symmetric-eigen pullbacks** remove reverse-time eigensolves from
  `kronecker_gp`: the targeted median falls 289.0 -> 185.7 us/gradient. Its
  current warmed mean is 184.6 us, 1.18x CmdStan.
- **Native Cholesky-density partials** cover the exact single-observation,
  Cholesky-factor-active `multi_normal_cholesky` shape in `gp_regr`: the
  targeted median falls 6.05 -> 4.20 us/gradient. Its current warmed mean is
  4.33 us, 1.09x CmdStan.
- **Elementwise-lp fusion** (the mixture idiom): `low_dim_gauss_mix`
  7,208 ops -> 16, crossing parity (0.78x -> 1.07x); `normal_mixture`
  13 ops, 1.09x.

## Tape islands, measured

The island pass compiles irreducible scalar residue (recurrences) into
one register-machine op. For most of this pass's life the op collapse was
dramatic on every model with such a region and the time followed on
exactly one, because the backward re-executed the whole program under
`stan::math::var`: a vari allocated per operation, a virtual `chain()`
per operation, a nested tape built and torn down per call. That is
correct by construction and it costs what CmdStan costs, so the island
bought data movement and nothing else -- `iohmm_reg`, whose steps copy a
1,500-element state vector, won 2.5x, and the estimate refused nearly
everything else.

`gen_adjoint` (`runtime/src/adjoint.cpp`) generates the backward instead:
reverse-mode source transformation over the ~35 opcodes of `Program`,
producing a second pass over doubles with no vari, no nested tape and no
allocation. Measured with `STANLI_NO_ISLAND=1` as the baseline, same
build, same point, on all twenty-one corpus models that compile a
region (`harnesses/island_ab.py`, min of three runs each -- the sweep
bypasses the carve estimate so the regions it declines are measured
too, which is how the table can hold rows the default build refuses):

| model | ops off -> on | ns/grad off | replayed | generated | |
| --- | ---: | ---: | ---: | ---: | ---: |
| `iohmm_reg` | 53,456 -> 27 | 1,428,776 | 574,888 (2.49x) | 301,629 | **4.74x** |
| `hmm_gaussian` | 42,926 -> 11 | 365,750 | 398,088 (0.92x) | 228,638 | **1.60x** |
| `hmm_example` | 3,483 -> 13 | 32,292 | 36,465 (0.89x) | 20,766 | **1.56x** |
| `hmm_drive_1` | 19,540 -> 24 | 171,084 | 199,678 (0.86x) | 121,344 | **1.41x** |
| `hmm_drive_0` | 19,540 -> 24 | 162,653 | 195,899 (0.83x) | 117,285 | **1.39x** |
| `garch11` | 1,797 -> 8 | 10,996 | 14,777 (0.74x) | 8,109 | **1.36x** |
| `Mb_model` | 7,035 -> 1,646 | 72,289 | 71,934 (1.00x) | 65,250 | **1.11x** |
| `arma11` | 1,205 -> 9 | 6,774 | 10,526 (0.64x) | 6,544 | 1.04x |
| `accel_gp` | 461 -> 64 | 7,233 | 8,170 (0.89x) | 7,041 | 1.03x |
| `losscurve_sislob` | 316 -> 26 | 2,340 | 3,165 (0.74x) | 2,281 | 1.03x |
| `multi_occupancy` | 4,006 -> 3,659 | 68,098 | 70,760 (0.96x) | 68,097 | 1.00x |
| `hier_2pl` | 349 -> 97 | 301,507 | 302,117 (1.00x) | 301,792 | 1.00x |
| `soil_incubation` | 129 -> 32 | 96,445 | 96,704 (1.00x) | 96,903 | 1.00x |
| `kronecker_gp` | 254 -> 166 | 302,779 | 315,098 (0.96x) | 306,665 | 0.99x |
| `accel_splines` | 425 -> 28 | 7,745 | 8,963 (0.86x) | 7,882 | 0.98x |
| `hierarchical_gp` | 165 -> 84 | 30,448 | 35,607 (0.86x) | 31,041 | 0.98x |
| `covid19imperial_v3` | 21,526 -> 19,995 | 308,879 | 439,340 (0.70x) | 316,736 | 0.98x |
| `covid19imperial_v2` | 21,526 -> 19,995 | 305,642 | 442,001 (0.69x) | 315,311 | 0.97x |
| `Survey_model` | 1,427 -> 5 | 61,524 | 62,030 (0.99x) | 65,039 | 0.95x |
| `dugongs_model` | 120 -> 12 | 768 | 766 (1.00x) | 1,168 | 0.66x |
| `bones_model` | 7,528 -> 4,955 | 52,335 | 988,998 (0.05x) | 207,721 | 0.25x |

Three of the twenty-one exist because the machine's vocabulary stopped
being a subset of the graph's: any scalar-out op it has no instruction
for now compiles as a CALL to the graph's own kernel -- the identical
code, partials, and backward the op would have run -- so one such op no
longer ends a run (`POW` used to split regions in half). A CALL buys
continuity, never speed, and the estimate charges it the graph's own
per-op tax; without that charge the first sweep carved `dugongs_model`
at a measured 0.66x and `Survey_model` at 0.95x, and with it both are
refused on the default path while every previously carved verdict is
unchanged. `losscurve_sislob` is the payoff shape: its residue drops
88 -> 26 ops because the cdfs inside it stopped ending the run.

Every region is faster generated than replayed, and the class changed
rather than improved: op collapse is now worth roughly what the op counts
always suggested it should be. `hmm_gaussian` collapses 42,926 ops to 11
and measured 0.92x replayed against 1.60x generated. The ceiling is
parity-plus and not more, as predicted before measuring -- CmdStan's
generated code is inlined compiled C++ and the adjoint program still pays
interpreted dispatch per instruction -- and `iohmm_reg` beats it only
because the registers also make its vector copies disappear.

The estimate changed with it. It weighed the register file 4x because the
file was built as vars, and that term is what refused thirteen of the
fourteen regions it could compile. A value register is a memory cost again:
one forward write and one backward read. The adjoint file is a separate
cost, however. Checkpoints hold values only, and registers made equivalent
by a copy already share an adjoint cell to reproduce stan-math's tape order.
Those equivalence classes are now packed densely, so the runtime zeroes one
cell per distinct class rather than one per value or checkpoint register.
The estimate mirrors the storage exactly: two passes over value and
checkpoint registers other than CALL scratch, one pass over the compact
adjoint file, both instruction streams, and the existing neutral charge for
CALLs.

What an island buys is still mostly the per-op tax the graph pays -- a
dispatch, a context load and a scratch-partials backward, ~5 ns against
~1 ns for an island instruction (`kOpCost = 5`). Without it a region like
`garch11`, whose scalar ops barely move more elements than there are ops,
reads as a wash. The 2026-08-24 structural census reran default, disabled,
and forced islands on all 21 corpus models with compilable regions. Compact
accounting changed exactly one decision, `iohmm_reg`, and preserved every
previous selection and refusal, including the measured loss guards
`bones_model`, `dugongs_model`, `Survey_model`, and both `covid19imperial`
models. `STANLI_ISLAND_ALWAYS=1` skips the estimate, which is how to ask why
a region was left alone.

In the targeted Release A/B from that census, `iohmm_reg`'s 95,424 forward
register ids reduce to 39,000 distinct adjoint cells; 4,488 additional
checkpoint registers remain value-only. Its estimated island cost falls
from 389,640 to 328,728 against the graph's 361,045, so the default path now
collapses 53,456 ops to 27. Seven clean, interleaved runs moved the median
from 498.612 to 241.453 us/gradient (2.065x internally), within 0.3% of the
forced-island path and 1.33x faster than the retained 320.335 us CmdStan
reference. This is a targeted A/B; the warmed corpus table above remains the
last full-corpus run until the next benchmark refresh.

`STANLI_NO_NATIVE_ADJ=1` restores the replay. It changes nothing else --
the adjoint is still generated, the estimate still assumes it, and the
forward program is identical -- so the two backwards are compared over
the same islands, which is what the "replayed" column above is.

Fifteen of the eighteen agree with the replay **bitwise**; each rule is
the corresponding stan-math rev expression transcribed, grouping
included. The other three reassociate one sum: a var copy shares a vari,
so from the copy onward both registers accumulate into one adjoint, and
`gen_adjoint` shares an adjoint cell to match -- but a cell is shared for
the whole program where a vari is shared only until the destination is
next written. Where they differ the arbiter is the op graph the island
replaced, not the replay, and the generated adjoint is closer to it in
all three: `Mb_model` reproduces the op graph exactly where the replay
was 1.07e-14 away, and `iohmm_reg` is 3.46e-13 against the replay's
6.01e-13.

Beyond the estimate, islands still refuse propto densities (their
term-dropping depends on argument types, which the island's uniform
binding cannot reproduce), runs under 32 ops, and regions producing
target terms. `gen_adjoint` additionally refuses jumps, so the regions
lowering emits for parameter-dependent control flow keep the replay:
reversing control flow wants the structured form the flat instruction
list has already lost.

## ODE models

The before/after numbers in this section are targeted historical A/B medians;
the current warmed-mean corpus rows are called out separately.

Every user function is inlined at lowering time except an ODE
right-hand side, which the integrator calls at times of its choosing.
It used to be evaluated by a tree-walking interpreter: 5.8 us per call,
~500 calls per gradient, 97% of the model's gradient time, and the
system was solved twice per gradient (values, then derivatives). Both
are gone: the right-hand side compiles once into a flat register
machine, and the forward sweep keeps the sensitivities it already
computes, so the backward is a matrix-vector product.

| model | before | after | speedup | vs CmdStan |
| --- | ---: | ---: | ---: | ---: |
| `lotka_volterra` | 2,790,941 ns | 71,704 ns | 38.9x | 0.015x -> 0.58x |
| `soil_incubation` | 3,389,538 ns | 96,362 ns | 35.2x | 0.018x -> 0.63x |
| `one_comp_mm_elim_abs` | 18,873,857 ns | 653,181 ns | 28.9x | 0.025x -> 0.74x |

Gradients are unchanged to the bit where they were before, and
`lotka_volterra` moved from 4 ULP to bitwise identical to CmdStan:
reading the jacobian out of the same solve that produced the values
removes a second, independently stepped solve. Anything the compiler
cannot express keeps the interpreter, so coverage never shrinks;
`STANLI_DEBUG_ODE=1` reports when that happens.

A later mixed-activity specialization also preserves Stan Math's separate
scalar types for the initial state and parameters. Previously either active
input promoted both to reverse mode and integrated sensitivities for both.
On `one_comp_mm_elim_abs` the initial state is data, so the sensitivity width
falls from four to three and median latency from 699 to 639 us/gradient. The
fully active `lotka_volterra` and `soil_incubation` shapes take the same path
as before and remain flat within measurement noise. The retained pre-patch
full-corpus warmed means are 629 us for `one_comp_mm_elim_abs`, 78 us for
`lotka_volterra`, and 103 us for `soil_incubation`, or 0.75x, 0.53x, and
0.59x CmdStan.

Preparation scales too: the largest corpus model (`nn_rbm1bJ100`, MNIST,
60,000 rows, 79,411 parameters) lowers to a 132,024-op graph. Its old 23.80 s
compile was almost entirely stanc's generated loop reconstructing the
47-million-element input matrix after `DataMap` had already parsed it. Direct
typed input preload removes that loop; an indexed reroll candidate scan removes
another empty 0.13 s pass over the resulting graph. Together they reduce graph
compilation to 0.23 s (103x), and the full MIR/data-to-bound-executor path from
26.33 s to 2.76 s (9.5x); JSON parsing is now the largest preparation stage.
The same profiled A/B removes 1.34 GB of peak RSS. The log density and all
79,411 gradient components remain within the existing CmdStan oracle
tolerance. Overall, graph
compilation is 4-400 ms against a 6.2-7.6 s CmdStan compile (warm precompiled
header, after a multi-minute one-time `make build`); that gap is what
time-to-first-draw is made of. The current corpus run records 3.158 s for the
same `nn_rbm1bJ100` MIR/data-to-bound-executor path.

## End to end: eight schools, 1000 warmup + 1000 draws

| engine | stage | time |
| --- | --- | --- |
| stanli | stanc + graph compile + bind | 0.014 s |
| stanli | NUTS 1000+1000 (incl. constraining draws) | 0.020 s |
| stanli | CLI total (`stanli_run`, process start to CSV) | 0.24 s |
| CmdStan | model build (stanc + clang, warm PCH) | 4.62 s |
| CmdStan | NUTS 1000+1000 (self-reported total) | 0.044 s |
| CmdStan | build + run wall time | ~4.98 s |

Time-to-first-draw is ~20x faster (0.24 s vs ~4.98 s). Treat sampling
times as indicative rather than controlled: adaptation trajectories
legitimately differ between engines at matched seeds, so they measure
the whole run, not the gradient. (An earlier revision of the sampling
numbers was measured with a defective max tree depth of 5; the columns
in `docs/corpus-bench.tsv` and the table below have been re-measured at
the correct depth.)

## Parallel chains, and what STAN_THREADS costs

Chains run in threads, one executor each. The tax worth measuring:
stan-math's autodiff stack becomes thread-local under `STAN_THREADS`,
and that indirection lands on every var operation, so the model to
worry about is one dominated by the nested-tape path.

| model | plain | `STAN_THREADS` |
| --- | ---: | ---: |
| 200-step `ordered_logistic` recurrence (72% in a legacy op) | 44,695 ns | 44,370 ns |
| `es` (eight schools, native kernels) | 221.4 ns | 223.0 ns |
| `ar1` | 228.9 ns | 224.1 ns |
| `conj` | 142.2 ns | 141.1 ns |

The tax is noise, in both directions, including on the model built to
expose it. Against that, eight chains of that model:

| threads | 1 | 2 | 4 | 8 |
| --- | ---: | ---: | ---: | ---: |
| 8 chains, wall clock | 2.89 s | 1.59 s | 0.86 s | 0.49 s |

So it is on by default for native builds, and off under Emscripten
(wasm threads need a cross-origin-isolated page; the browser runs one
chain per worker instead).

**Threading does not change the answer.** Each chain owns its executor
and its RNG stream, so a parallel run is byte-identical to a sequential
one. Checked as a CSV `cmp` across four models at 8 chains x 300 draws,
and asserted in `tests/test_multichain.cpp` and `tests/test_python.py`.

One trap, worth naming because it is invisible from the outside:
stan-math's AD stack pointer is thread-local under `STAN_THREADS` and
starts **null** in every new thread. CmdStan never has to handle this
because TBB's scheduler hook instantiates a `ChainableStack` per
worker; this build stubs TBB out, so raw `std::thread`s must do it
themselves or segfault on the first legacy op.

## Numerical parity

Every model in the passing set is differentially verified against
CmdStan's `log_prob_propto_jacobian` and full gradient at the shared
point: 118/120 verified, 41 of them bitwise identical, worst relative
deviation 2.6e-12 (`tools/verify_sample.py`,
`docs/corpus-status.md`). For the 20 models whose generated quantities
are deterministic, the same oracle also replays CmdStan's write_array
values (every CSV column at the same point); recording those caught two
interpreter bugs invisible to structural coverage checks.

Transformed models change summation order relative to CmdStan's scalar
loop, so they verify at tolerance rather than bitwise: the passes
change 65 corpus models, and the worst gradient deviation any of them
introduces vs the untransformed graph is 3.5e-13 relative -- `iohmm_reg`,
whose entire forward algorithm runs as one island. That was 6.0e-13 while
the island replayed under `var`: generating the backward moved it closer
to the graph it replaced rather than further, and `hmm_gaussian`, which
islands its whole forward algorithm too, now reproduces the untransformed
graph exactly. (`harnesses/ab_corpus.py` compares every model passes-on
vs passes-off and flags any divergence.) That harness caught an in-place
rule that made eight models silently wrong by up to 1.7e+05 relative with
their op counts unchanged; the six models most affected by the passes
were then also re-run through the CmdStan rig directly, and all six
verify (e.g. `radon_pooled` 2.1e-14, `arK` 9.6e-16, `rats_model`
2.4e-16).

## Full corpus

Every posteriordb model, sorted by per-gradient speedup. The columns are
CmdStan's absolute numbers and stanli's ratio against them. The stanli
gradient and sampling columns are the 2026-08-25 refresh; the unchanged
CmdStan columns carry over from the earlier run on the same host.

Sampling is 1000 warmup + 1000 draws at matched seeds and is indicative
rather than controlled. Tiny numerical differences can send NUTS down
different trajectories, and matched seeds do not guarantee matched modes.
Across the 117 rows completed by both engines, the median end-to-end
speedup is 2.25x and 110 finish at or above CmdStan's time for this seed.
For example, CmdStan's `hmm_gaussian` run at this seed has every post-warmup
draw divergent, so its 18.75 s is not a useful sampler comparison. Read the
gradient column as controlled fixed-point throughput and the sampling column
as end-to-end behavior for this one seed. Regenerate both tables with
`python3 tools/corpus_table.py docs/corpus-bench.tsv`.

| model | params | CmdStan ns/grad | grad speedup | CmdStan sample | sample speedup |
| --- | ---: | ---: | ---: | ---: | ---: |
| `arK` | 7 | 12,459 | 7.10x | 0.97 s | 6.06x |
| `dogs` | 3 | 63,747 | 7.01x | 2.39 s | 3.68x |
| `logmesquite_logvash` | 7 | 2,841 | 6.59x | 0.39 s | 3.25x |
| `logmesquite_logvas` | 8 | 3,130 | 6.58x | 0.42 s | 3.23x |
| `radon_pooled` | 3 | 320,938 | 6.33x | 3.78 s | 6.20x |
| `mesquite` | 8 | 3,055 | 6.31x | 0.57 s | 3.00x |
| `logmesquite` | 8 | 2,902 | 5.96x | 0.33 s | 4.12x |
| `logmesquite_logva` | 5 | 2,070 | 5.91x | 0.29 s | 3.62x |
| `radon_hierarchical_intercept_centered` | 391 | 569,143 | 5.87x | 44.69 s | 4.80x |
| `radon_county_intercept` | 388 | 431,614 | 5.27x | 27.37 s | 5.08x |
| `GLM_Poisson_model` | 4 | 2,008 | 5.19x | 0.24 s | 3.43x |
| `rats_model` | 65 | 6,475 | 5.11x | 0.52 s | 2.48x |
| `logmesquite_logvolume` | 3 | 1,304 | 5.00x | 0.20 s | 5.00x |
| `Rate_2_model` | 2 | 561 | 4.75x | 0.23 s | 7.67x |
| `radon_hierarchical_intercept_noncentered` | 391 | 570,300 | 4.74x | 56.03 s | 4.53x |
| `radon_variable_intercept_centered` | 390 | 427,262 | 4.72x | 23.02 s | 4.22x |
| `radon_variable_intercept_noncentered` | 390 | 430,721 | 4.69x | 33.78 s | 4.08x |
| `kilpisjarvi` | 3 | 1,532 | 4.60x | 1.60 s | 1.67x |
| `radon_variable_slope_centered` | 390 | 420,987 | 4.56x | 23.66 s | 4.19x |
| `radon_variable_slope_noncentered` | 390 | 422,894 | 4.48x | 51.94 s | 4.24x |
| `dogs_log` | 2 | 41,387 | 4.37x | 1.01 s | 1.87x |
| `nes` | 10 | 69,324 | 4.32x | 6.70 s | 4.38x |
| `kidscore_interaction_c` | 5 | 10,333 | 4.19x | 0.39 s | 4.33x |
| `Rate_1_model` | 1 | 260 | 4.13x | 0.15 s | 7.50x |
| `kidscore_interaction_z` | 5 | 10,013 | 4.03x | 0.47 s | 4.27x |
| `kidscore_mom_work` | 5 | 9,959 | 4.03x | 0.64 s | 4.57x |
| `kidscore_interaction_c2` | 5 | 9,901 | 4.01x | 0.40 s | 4.44x |
| `kidscore_interaction` | 5 | 9,927 | 3.98x | 1.90 s | 3.45x |
| `GLMM_Poisson_model` | 45 | 2,412 | 3.59x | 0.69 s | 3.29x |
| `radon_partially_pooled_centered` | 389 | 272,243 | 3.58x | 13.96 s | 3.19x |
| `radon_partially_pooled_noncentered` | 389 | 273,685 | 3.57x | 20.11 s | 3.41x |
| `sesame_one_pred_a` | 3 | 3,440 | 3.50x | 0.23 s | 3.83x |
| `kidscore_momhsiq` | 4 | 7,145 | 3.49x | 0.84 s | 3.23x |
| `GLMM1_model` | 237 | 35,558 | 3.46x | 1.68 s | 1.56x |
| `logearn_interaction_z` | 5 | 26,484 | 3.45x | 0.84 s | 3.82x |
| `logearn_interaction` | 5 | 26,051 | 3.40x | 8.49 s | 3.25x |
| `Rate_4_model` | 2 | 311 | 3.31x | 0.17 s | 5.67x |
| `Rate_3_model` | 1 | 268 | 3.27x | 0.18 s | 9.00x |
| `radon_variable_intercept_slope_centered` | 777 | 437,889 | 3.22x | 27.23 s | 2.65x |
| `radon_variable_intercept_slope_noncentered` | 777 | 441,463 | 3.22x | 58.56 s | 2.94x |
| `logearn_logheight_male` | 4 | 18,697 | 3.22x | 13.41 s | 3.07x |
| `kidscore_momiq` | 3 | 4,861 | 3.18x | 0.42 s | 2.80x |
| `logearn_height_male` | 4 | 19,147 | 3.17x | 3.77 s | 2.65x |
| `election88_full` | 90 | 901,961 | 3.09x | 468.15 s | 4.77x |
| `Rate_5_model` | 1 | 262 | 3.05x | 0.19 s | 9.50x |
| `blr` | 6 | 1,728 | 2.97x | 0.21 s | 3.50x |
| `seeds_centered_model` | 26 | 2,650 | 2.91x | 0.28 s | 2.33x |
| `kidscore_momhs` | 3 | 4,483 | 2.90x | 0.30 s | 4.29x |
| `bym2_offset_only` | 3845 | 114,620 | 2.83x | 23.40 s | 1.39x |
| `seeds_stanified_model` | 26 | 2,341 | 2.70x | 0.30 s | 2.73x |
| `log10earn_height` | 3 | 11,560 | 2.64x | 1.75 s | 1.79x |
| `logistic_regression_rhs` | 3075 | 113,106 | 2.61x | 16.23 s | 1.38x |
| `surgical_model` | 14 | 1,684 | 2.55x | 0.23 s | 2.88x |
| `logearn_height` | 3 | 11,162 | 2.55x | 1.71 s | 2.38x |
| `eight_schools_noncentered` | 10 | 745 | 2.47x | 0.20 s | 4.00x |
| `seeds_model` | 26 | 2,130 | 2.38x | 0.29 s | 2.42x |
| `pilots` | 18 | 1,878 | 2.27x | 1.29 s | 1.63x |
| `irt_2pl` | 144 | 37,468 | 2.24x | 2.17 s | 1.63x |
| `earn_height` | 3 | 10,866 | 2.21x | 1.86 s | 2.11x |
| `GLM_Binomial_model` | 3 | 1,809 | 2.17x | 0.21 s | 3.00x |
| `lsat_model` | 1006 | 91,173 | 2.08x | 4.66 s | 1.60x |
| `dugongs_model` | 4 | 1,653 | 1.75x | 0.25 s | 2.78x |
| `wells_dist` | 2 | 39,202 | 1.75x | 1.44 s | 2.06x |
| `Mth_model` | 394 | 93,922 | 1.65x | 5.43 s | 1.53x |
| `2pl_latent_reg_irt` | 531 | 134,556 | 1.61x | 7.94 s | 1.24x |
| `losscurve_sislob` | 15 | 3,450 | 1.60x | 0.31 s | 1.41x |
| `Mtbh_model` | 154 | 42,791 | 1.59x | 2.38 s | 1.53x |
| `hierarchical_gp` | 933 | 47,565 | 1.51x | 17.20 s | 1.15x |
| `grsm_latent_reg_irt` | 408 | 762,133 | 1.50x | 66.92 s | 2.33x |
| `accel_splines` | 82 | 10,584 | 1.45x | 19.74 s | 1.21x |
| `accel_gp` | 66 | 9,532 | 1.42x | 16.99 s | 1.33x |
| `covid19imperial_v2` | 51 | 345,937 | 1.41x | 176.00 s | 1.56x |
| `state_space_stochastic_level_stochastic_seasonal` | 389 | 26,320 | 1.41x | 39.76 s | 0.90x |
| `garch11` | 4 | 9,664 | 1.37x | 0.43 s | 2.15x |
| `arma11` | 4 | 6,158 | 1.35x | 0.26 s | 2.89x |
| `M0_model` | 2 | 15,595 | 1.34x | 0.37 s | 2.47x |
| `hier_2pl` | 669 | 397,603 | 1.33x | 26.82 s | 1.46x |
| `hmm_example` | 4 | 27,145 | 1.32x | 1.00 s | 1.49x |
| `gpcm_latent_reg_irt` | 530 | 1,337,651 | 1.32x | 161.93 s | 2.25x |
| `iohmm_reg` | 29 | 320,335 | 1.31x | 181.23 s | 1.77x |
| `normal_mixture_k` | 14 | 357,439 | 1.27x | 101.61 s | 1.41x |
| `nes_logit_model` | 2 | 7,653 | 1.25x | 0.38 s | 2.38x |
| `prophet` | 62 | 69,789 | 1.23x | 117.68 s | 1.26x |
| `hmm_gaussian` | 14 | 263,917 | 1.19x | 18.75 s | 0.05x |
| `kronecker_gp` | 438 | 217,990 | 1.18x | 451.09 s | 1.12x |
| `hmm_drive_1` | 6 | 147,829 | 1.17x | 6.94 s | 1.33x |
| `covid19imperial_v3` | 51 | 342,943 | 1.17x | 175.70 s | 1.54x |
| `radon_county` | 389 | 82,076 | 1.12x | 4.49 s | 1.11x |
| `low_dim_gauss_mix_collapse` | 5 | 95,373 | 1.11x | 4.45 s | 1.11x |
| `hmm_drive_0` | 6 | 132,850 | 1.10x | 3.65 s | 1.17x |
| `wells_dist100ars_model` | 3 | 18,997 | 1.09x | 0.62 s | 1.48x |
| `low_dim_gauss_mix` | 5 | 98,315 | 1.09x | 1.98 s | 1.37x |
| `ldaK2` | 7 | 104,059 | 1.09x | 3.19 s | 1.38x |
| `gp_regr` | 3 | 4,698 | 1.08x | 0.23 s | 2.88x |
| `wells_interaction_c_model` | 4 | 20,272 | 1.07x | 0.49 s | 1.81x |
| `wells_interaction_model` | 4 | 20,402 | 1.07x | 0.94 s | 1.34x |
| `Mt_model` | 4 | 19,984 | 1.06x | 0.48 s | 1.78x |
| `wells_dist100_model` | 2 | 17,195 | 1.05x | 0.47 s | 1.81x |
| `normal_mixture` | 3 | 88,239 | 1.03x | 1.13 s | 1.24x |
| `Survey_model` | 1 | 61,578 | 1.02x | 1.14 s | 1.11x |
| `diamonds` | 26 | 31,497 | 1.01x | 48.55 s | 0.86x |
| `wells_dae_model` | 4 | 20,356 | 1.00x | 0.76 s | 1.33x |
| `bones_model` | 13 | 51,501 | 1.00x | 1.31 s | 1.47x |
| `gp_pois_regr` | 13 | 3,935 | 0.99x | 1.47 s | 1.04x |
| `wells_dae_c_model` | 5 | 19,308 | 0.99x | 0.59 s | 1.34x |
| `wells_dae_inter_model` | 7 | 21,310 | 0.97x | 0.55 s | 1.53x |
| `nn_rbm1bJ10` | 7951 | 185,731 | 0.97x | 456.82 s | 0.90x |
| `dogs_hierarchical` | 2 | 34,053 | 0.94x | 0.68 s | 1.17x |
| `wells_daae_c_model` | 6 | 20,885 | 0.93x | 0.63 s | 1.11x |
| `eight_schools_centered` | 10 | 314 | 0.93x | 0.19 s | 3.17x |
| `Mb_model` | 3 | 49,570 | 0.93x | 1.15 s | 1.01x |
| `Mh_model` | 388 | 38,956 | 0.88x | 2.66 s | 1.04x |
| `dogs_nonhierarchical` | 65 | 40,588 | 0.78x | 2.86 s | 1.11x |
| `multi_occupancy` | 106 | 58,996 | 0.78x | 7.34 s | 0.79x |
| `one_comp_mm_elim_abs` | 4 | 470,681 | 0.76x | 11.23 s | 1.08x |
| `soil_incubation` | 6 | 60,871 | 0.59x | 12.84 s | 0.57x |
| `lotka_volterra` | 8 | 41,313 | 0.59x | 6.24 s | 0.91x |

120 models; 119 with both gradients; median per-gradient speedup 2.17x; 104/119 at or above CmdStan.

### The models the run could not complete

A missing sampling number is not a missing gradient. These rows sort below the
complete table, and measured gradient ratios still stand.

| model | params | CmdStan ns/grad | grad speedup | what stopped it |
| --- | ---: | ---: | ---: | --- |
| `ldaK5` | 7714 | 5,580,314 | 1.49x | stanli sampling hit the 900 s cap; CmdStan sampling hit the 900 s cap |
| `nn_rbm1bJ100` | 79411 | 434,981,254 | 1.05x | stanli sampling hit the 900 s cap; CmdStan sampling hit the 900 s cap |
| `sir` |  | - | - | stanli's gradient probe threw at the benchmark point; no stanli gradient |

`ldaK5` and `nn_rbm1bJ100` are the two preparation outliers; both per-gradient
measurements completed even though both samplers hit the 900 s cap. `sir` has
no gradient number because the fixed probe point makes its ODE solution dip
to -4.4e-10 and `poisson_lpmf` rejects the negative rate; the model itself
samples successfully.

## The browser build

Different compiler, different libm, so none of the numbers above carry
over. `tools/bench_wasm.cjs` measures it under Node against the same
MIR fixtures the tests use; `--mir`/`--data` point it at a bigger
model. Measure under Node, never in an automated browser tab: attaching
chrome.debugger keeps V8 on its baseline tier and costs about 4x.

### What the browser costs you

Both builds from the same commit, same MIR and data, min of three runs,
macOS arm64. This is the number the demo page's footer cites, and the
reason it tells people to install the wheel for real work.

| model | native | wasm | wasm/native |
|---|---:|---:|---:|
| `eight_schools_noncentered` | 269 ns | 534 ns | 1.98x |
| `radon_pooled` | 54.3 us | 106.6 us | 1.96x |
| `low_dim_gauss_mix` | 93.2 us | 168.4 us | 1.81x |
| `hmm_example` | 33.8 us | 58.6 us | 1.73x |
| `arK` | 2.32 us | 3.75 us | 1.62x |
| `lotka_volterra` | 74.6 us | 110.6 us | 1.48x |
| `diamonds` | 36.3 us | 52.2 us | 1.44x |
| geometric mean | | | **1.71x** |

The gap is gradient throughput, which is what dominates once sampling
starts. It is not model preparation: stanc3 compiled by js_of_ocaml
compiles eight schools in 3 ms warm, against 13 ms for the native
binary including process spawn.

### SIMD

SIMD128 is on. The A/B that decided it (macOS arm64, emsdk 6.0.6, on
the tree as it stood then; sizes have grown since as the density
surface roughly doubled):

| | scalar | +SIMD128 |
|---|---:|---:|
| fixtures, geomean | 457 ns | **449 ns** |
| `radon_pooled` (919 obs, vectorized normal) | 107.9 us | **105.7 us** |
| `nes` (matrix-heavy) | 29.1 us | **25.9 us** |
| `stanli.wasm` gzipped | 1.02 MB | 1.05 MB |

2% on most shapes and 11% on the matrix-heavy one for 0.03 MB, and
(the part that made it an easy call) every corpus model's gradients are
**bitwise identical** to the scalar build. With `-ffp-contract=off` in
force the vectorization Eigen takes is elementwise, and elementwise is
order-preserving; a vectorized reduction would have shown up as a
deviation immediately.

Two related results, measured: `-ffp-contract=fast` produces a
byte-identical `stanli.wasm`, because baseline WebAssembly has no FMA
instruction to contract to, so fast-math is not a lever on this target.
And splitting the densities into a lazily-loaded pack is viable but
blocked by one emscripten limitation; the measurements and traps are in
[docs/density-pack.md](density-pack.md).

## Reproducing

```
./tools/dev_setup.sh --corpus          # deps, build, posteriordb, CmdStan
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  docs/corpus-bench.tsv --stanli-only --timeout 900
# To remeasure both sides, omit --stanli-only and use a new output TSV.
python3 harnesses/ab_corpus.py deps/posteriordb   # passes A/B over the corpus
```
