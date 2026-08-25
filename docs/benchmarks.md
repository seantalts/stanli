# Benchmarks vs CmdStan

2026-08-24, Apple M3 Ultra (macOS arm64), Apple clang 21, both sides `-O3`
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
| `radon_pooled` | 3 | 45,516 | 320,938 | 7.05x | 0.015 s |
| `arK` | 7 | 1,776 | 12,459 | 7.02x | 0.002 s |
| `radon_hierarchical_intercept_centered` | 391 | 97,135 | 569,143 | 5.86x | 0.042 s |
| `radon_county_intercept` | 388 | 81,516 | 431,614 | 5.29x | 0.026 s |
| `nes` | 10 | 16,139 | 69,324 | 4.30x | 0.004 s |
| `eight_schools_noncentered` | 10 | 267 | 745 | 2.79x | 0.000 s |
| `election88_full` | 90 | 256,132 | 901,961 | 3.52x | 0.121 s |
| `bym2_offset_only` | 3845 | 40,175 | 114,620 | 2.85x | 0.002 s |
| `dogs` | 3 | 8,006 | 63,747 | 7.96x | 0.010 s |
| `kidscore_momiq` | 3 | 1,530 | 4,861 | 3.18x | 0.001 s |
| `lsat_model` | 1006 | 41,949 | 91,173 | 2.17x | 0.003 s |
| `state_space_stochastic_level_stochastic_seasonal` | 389 | 17,344 | 26,320 | 1.52x | 0.002 s |
| `hmm_example` | 4 | 21,042 | 27,145 | 1.29x | 0.003 s |
| `garch11` | 4 | 6,934 | 9,664 | 1.39x | 0.002 s |
| `hmm_drive_0` | 6 | 119,749 | 132,850 | 1.11x | 0.014 s |
| `normal_mixture` | 3 | 79,768 | 88,239 | 1.11x | 0.003 s |
| `low_dim_gauss_mix` | 5 | 90,712 | 98,315 | 1.08x | 0.003 s |
| `wells_dist100ars_model` | 3 | 17,019 | 18,997 | 1.12x | 0.002 s |
| `iohmm_reg` | 29 | 492,902 | 320,335 | 0.65x | 0.183 s |
| `radon_county` | 389 | 73,425 | 82,076 | 1.12x | 0.011 s |
| `arma11` | 4 | 4,474 | 6,158 | 1.38x | 0.001 s |
| `diamonds` | 26 | 31,202 | 31,497 | 1.01x | 0.021 s |
| `ldaK2` | 7 | 99,542 | 104,059 | 1.05x | 0.005 s |

Generated adjoints put most sequential models in this slice at parity or
better: `arma11` and `garch11` are 1.38-1.39x, and the HMM rows other than
`iohmm_reg` are 1.11-1.29x. `iohmm_reg` remains at 0.65x in this warmed-mean
snapshot. The island section below preserves the targeted per-region A/B that
isolates what generated adjoints bought independently of absolute run noise.

## Which models are faster, which are slower, and why

Across the full corpus (`docs/corpus-bench.tsv`, 119 models with both
gradients) the median per-gradient speedup is 2.18x and 109 of 119
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
  into the class above: the radon family up to 7.1x, `election88_full`
  3.5x, `dogs` 8.0x.
- **Nested fixed-width mixtures.** LDA's per-document `gamma[K]` construction
  and row `log_sum_exp` become two packed gathers, vector arithmetic, and one
  row-reduction op. In the current corpus run, `ldaK2` is 99.5 us/gradient
  (1.05x CmdStan) and `ldaK5` is 3.66 ms/gradient (1.52x).
- **Everything, on preparation.** The MIR/data-to-bound-executor path has a
  2 ms median; 82 of 119 measured models prepare in at most 5 ms. The largest
  JSON input, `nn_rbm1bJ100`, takes 2.722 s and `ldaK5` takes 0.270 s, against
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
  became a generated adjoint program: `hmm_example` is now 1.29x,
  `garch11` 1.39x, `hmm_gaussian` 1.14x, and the two `hmm_drive` models
  1.11-1.15x against CmdStan. What keeps them at parity-plus rather than
  higher is per-op dispatch against CmdStan's inlined scalar code, one
  interpreted instruction at a time in each direction (the island section
  below has the targeted per-region numbers). `iohmm_reg` is the exception in
  this full run at 0.65x.
- **Matrix-filling updates.** `Mtbh_model` re-rolls 584 element writes into
  146 strided stores. A second in-place pass now keeps one initial copy and
  makes all 146 stores destructive, reducing their profile share from 42.4%
  to 5.5%. Native Bernoulli forwards then remove recorder overhead; the
  current `Mt_model`, `Mth_model`, and `Mtbh_model` rows are 1.05x, 1.65x,
  and 1.59x CmdStan.

**Slower (a shrinking tail, mostly 0.5-0.9x):**

- **ODE models (retained full-corpus snapshot)**: `lotka_volterra`,
  `soil_incubation`, and `one_comp_mm_elim_abs` are 0.53x, 0.59x, and 0.75x.
  The remaining gap is our per-call dispatch of the compiled right-hand side
  against CmdStan's native C++ right-hand side inside the same underlying Stan
  Math integrator. (They were 0.015-0.025x before the right-hand side compiled;
  see below.)
- **Large scalar residue, occupancy, and latent-regression IRT shapes**:
  `iohmm_reg` is 0.65x, `gpcm_latent_reg_irt` and `grsm_latent_reg_irt` are
  0.74x and 0.79x, and `multi_occupancy` is 0.79x. These are all the non-ODE
  rows below 0.8x in this snapshot.

A profile of every sub-parity model (`STANLI_PROFILE=1`) says the
remaining tail is mostly not a graph problem: in seven of those models
a single precompiled kernel is half to nine-tenths of the gradient.
`diamonds` was the extreme, 90.9% in one GLM kernel that rebuilt a var
tape in both sweeps; differentiating once and stashing the partials took it
0.48x -> 0.89x in that targeted A/B; the current warmed mean is 1.01x.
`prophet` was 82% in `OP_MATVEC` with one serial accumulation chain; four
independent accumulators took it 0.67x -> 1.23x in the targeted A/B and it is
1.28x in the current corpus run, bitwise unchanged.

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
  are not replacements for the full-corpus warmed means in the table below,
  which await the next corpus refresh.
- **Compiled scalar generated-quantities RNGs** keep caller-owned chain state
  on the forward-only write-array graph for scalar `poisson_log`, `uniform`,
  `bernoulli`, `normal`, `lognormal`, and `binomial` draws.
  Unsupported/container RNGs and draws used as dynamic control, indices, or
  geometry still fail closed to the whole-section interpreter. In an exact
  census of the 24 previously interpreted corpus models, this RNG tranche
  moved 12 to the graph; all 24 still produced complete rows.
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
  to the graph, taking the current 24-model census from 12 graph / 12
  interpreter to 16 / 8, with all 119 compiling corpus models still producing
  complete rows. A targeted 2026-08-24 C-ABI A/B (point 0, two warmups, seven
  matched batch medians) measured:

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
  points. This targeted A/B attributes the direct-seeding change; it does not
  replace the retained full-corpus warmed means or CmdStan ratios above and in
  the table below, which await the next corpus refresh.
- **Packed row-wise reductions** (the LDA inner loop): targeted medians fall
  from 154 to 94 us for `ldaK2` and 6.82 to 3.70 ms for `ldaK5`, while their
  graphs collapse from 15,854 to 22 and 434,126 to 156 ops. The full
  warmed-mean rows are 99.5 us (1.05x) and 3.66 ms (1.52x).
- **Native symmetric-eigen pullbacks** remove reverse-time eigensolves from
  `kronecker_gp`: the targeted median falls 289.0 -> 185.7 us/gradient. Its
  current warmed mean is 184.9 us, 1.18x CmdStan.
- **Native Cholesky-density partials** cover the exact single-observation,
  Cholesky-factor-active `multi_normal_cholesky` shape in `gp_regr`: the
  targeted median falls 6.05 -> 4.20 us/gradient. Its current warmed mean is
  4.24 us, 1.11x CmdStan.
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
time-to-first-draw is made of. The current corpus run records 2.722 s for the
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
gradient and sampling columns are the 2026-08-24 refresh; the unchanged
CmdStan columns carry over from the earlier run on the same host.

Sampling is 1000 warmup + 1000 draws at matched seeds and is indicative
rather than controlled. Tiny numerical differences can send NUTS down
different trajectories, and matched seeds do not guarantee matched modes.
Across the 117 rows completed by both engines, the median end-to-end
speedup is 2.15x and 96 finish at or above CmdStan's time for this seed.
For example, CmdStan's `hmm_gaussian` run at this seed has every post-warmup
draw divergent, so its 18.75 s is not a useful sampler comparison. Read the
gradient column as controlled fixed-point throughput and the sampling column
as end-to-end behavior for this one seed. Regenerate both tables with
`python3 tools/corpus_table.py docs/corpus-bench.tsv`.

| model | params | CmdStan ns/grad | grad speedup | CmdStan sample | sample speedup |
| --- | ---: | ---: | ---: | ---: | ---: |
| `dogs` | 3 | 63,747 | 7.96x | 2.39 s | 4.19x |
| `radon_pooled` | 3 | 320,938 | 7.05x | 3.78 s | 6.87x |
| `arK` | 7 | 12,459 | 7.02x | 0.97 s | 6.06x |
| `logmesquite_logvash` | 7 | 2,841 | 6.64x | 0.39 s | 3.25x |
| `logmesquite_logvas` | 8 | 3,130 | 6.49x | 0.42 s | 3.50x |
| `mesquite` | 8 | 3,055 | 6.27x | 0.57 s | 3.00x |
| `logmesquite` | 8 | 2,902 | 6.00x | 0.33 s | 4.12x |
| `logmesquite_logva` | 5 | 2,070 | 5.88x | 0.29 s | 3.62x |
| `radon_hierarchical_intercept_centered` | 391 | 569,143 | 5.86x | 44.69 s | 5.34x |
| `radon_hierarchical_intercept_noncentered` | 391 | 570,300 | 5.85x | 56.03 s | 5.27x |
| `rats_model` | 65 | 6,475 | 5.82x | 0.52 s | 2.89x |
| `radon_county_intercept` | 388 | 431,614 | 5.29x | 27.37 s | 5.04x |
| `radon_variable_intercept_noncentered` | 390 | 430,721 | 5.27x | 33.78 s | 4.58x |
| `radon_variable_intercept_centered` | 390 | 427,262 | 5.23x | 23.02 s | 4.75x |
| `GLM_Poisson_model` | 4 | 2,008 | 5.16x | 0.24 s | 3.43x |
| `radon_variable_slope_noncentered` | 390 | 422,894 | 5.06x | 51.94 s | 4.82x |
| `radon_variable_slope_centered` | 390 | 420,987 | 5.05x | 23.66 s | 4.76x |
| `logmesquite_logvolume` | 3 | 1,304 | 4.98x | 0.20 s | 5.00x |
| `dogs_log` | 2 | 41,387 | 4.96x | 1.01 s | 2.15x |
| `Rate_2_model` | 2 | 561 | 4.75x | 0.23 s | 7.67x |
| `kilpisjarvi` | 3 | 1,532 | 4.70x | 1.60 s | 1.65x |
| `nes` | 10 | 69,324 | 4.30x | 6.70 s | 4.35x |
| `kidscore_interaction_c` | 5 | 10,333 | 4.22x | 0.39 s | 4.33x |
| `Rate_1_model` | 1 | 260 | 4.13x | 0.15 s | 7.50x |
| `radon_partially_pooled_centered` | 389 | 272,243 | 4.09x | 13.96 s | 3.64x |
| `radon_partially_pooled_noncentered` | 389 | 273,685 | 4.07x | 20.11 s | 3.79x |
| `sesame_one_pred_a` | 3 | 3,440 | 4.05x | 0.23 s | 5.75x |
| `kidscore_interaction_z` | 5 | 10,013 | 4.03x | 0.47 s | 4.27x |
| `kidscore_mom_work` | 5 | 9,959 | 4.02x | 0.64 s | 4.92x |
| `kidscore_interaction` | 5 | 9,927 | 3.96x | 1.90 s | 3.45x |
| `kidscore_interaction_c2` | 5 | 9,901 | 3.95x | 0.40 s | 4.44x |
| `radon_variable_intercept_slope_noncentered` | 777 | 441,463 | 3.66x | 58.56 s | 3.33x |
| `radon_variable_intercept_slope_centered` | 777 | 437,889 | 3.66x | 27.23 s | 3.02x |
| `GLMM_Poisson_model` | 45 | 2,412 | 3.59x | 0.69 s | 3.29x |
| `logearn_interaction_z` | 5 | 26,484 | 3.54x | 0.84 s | 3.82x |
| `election88_full` | 90 | 901,961 | 3.52x | 468.15 s | 5.08x |
| `kidscore_momhsiq` | 4 | 7,145 | 3.51x | 0.84 s | 3.23x |
| `logearn_interaction` | 5 | 26,051 | 3.46x | 8.49 s | 3.32x |
| `GLMM1_model` | 237 | 35,558 | 3.35x | 1.68 s | 1.49x |
| `seeds_centered_model` | 26 | 2,650 | 3.33x | 0.28 s | 2.80x |
| `logearn_height_male` | 4 | 19,147 | 3.28x | 3.77 s | 2.73x |
| `Rate_5_model` | 1 | 262 | 3.27x | 0.19 s | 6.33x |
| `Rate_4_model` | 2 | 311 | 3.24x | 0.17 s | 5.67x |
| `logearn_logheight_male` | 4 | 18,697 | 3.22x | 13.41 s | 3.05x |
| `Rate_3_model` | 1 | 268 | 3.19x | 0.18 s | 9.00x |
| `kidscore_momiq` | 3 | 4,861 | 3.18x | 0.42 s | 2.62x |
| `seeds_stanified_model` | 26 | 2,341 | 3.03x | 0.30 s | 3.00x |
| `blr` | 6 | 1,728 | 3.00x | 0.21 s | 3.50x |
| `kidscore_momhs` | 3 | 4,483 | 2.91x | 0.30 s | 4.29x |
| `surgical_model` | 14 | 1,684 | 2.90x | 0.23 s | 3.83x |
| `bym2_offset_only` | 3845 | 114,620 | 2.85x | 23.40 s | 1.40x |
| `log10earn_height` | 3 | 11,560 | 2.80x | 1.75 s | 1.84x |
| `eight_schools_noncentered` | 10 | 745 | 2.79x | 0.20 s | 5.00x |
| `seeds_model` | 26 | 2,130 | 2.66x | 0.29 s | 2.90x |
| `logistic_regression_rhs` | 3075 | 113,106 | 2.63x | 16.23 s | 1.39x |
| `logearn_height` | 3 | 11,162 | 2.62x | 1.71 s | 2.44x |
| `earn_height` | 3 | 10,866 | 2.57x | 1.86 s | 2.42x |
| `pilots` | 18 | 1,878 | 2.51x | 1.29 s | 1.70x |
| `irt_2pl` | 144 | 37,468 | 2.26x | 2.17 s | 1.63x |
| `GLM_Binomial_model` | 3 | 1,809 | 2.18x | 0.21 s | 3.00x |
| `lsat_model` | 1006 | 91,173 | 2.17x | 4.66 s | 1.66x |
| `dugongs_model` | 4 | 1,653 | 2.07x | 0.25 s | 3.12x |
| `wells_dist` | 2 | 39,202 | 1.85x | 1.44 s | 2.15x |
| `losscurve_sislob` | 15 | 3,450 | 1.67x | 0.31 s | 0.57x |
| `Mth_model` | 394 | 93,922 | 1.65x | 5.43 s | 1.18x |
| `2pl_latent_reg_irt` | 531 | 134,556 | 1.63x | 7.94 s | 1.24x |
| `Mtbh_model` | 154 | 42,791 | 1.59x | 2.38 s | 0.92x |
| `hierarchical_gp` | 933 | 47,565 | 1.55x | 17.20 s | 1.01x |
| `state_space_stochastic_level_stochastic_seasonal` | 389 | 26,320 | 1.52x | 39.76 s | 0.97x |
| `accel_splines` | 82 | 10,584 | 1.48x | 19.74 s | 1.22x |
| `accel_gp` | 66 | 9,532 | 1.44x | 16.99 s | 1.35x |
| `covid19imperial_v3` | 51 | 342,943 | 1.42x | 175.70 s | 0.68x |
| `garch11` | 4 | 9,664 | 1.39x | 0.43 s | 2.15x |
| `arma11` | 4 | 6,158 | 1.38x | 0.26 s | 2.89x |
| `M0_model` | 2 | 15,595 | 1.35x | 0.37 s | 2.31x |
| `covid19imperial_v2` | 51 | 345,937 | 1.34x | 176.00 s | 0.68x |
| `hier_2pl` | 669 | 397,603 | 1.34x | 26.82 s | 1.46x |
| `normal_mixture_k` | 14 | 357,439 | 1.33x | 101.61 s | 1.52x |
| `hmm_example` | 4 | 27,145 | 1.29x | 1.00 s | 0.50x |
| `prophet` | 62 | 69,789 | 1.28x | 117.68 s | 1.27x |
| `nes_logit_model` | 2 | 7,653 | 1.24x | 0.38 s | 2.38x |
| `kronecker_gp` | 438 | 217,990 | 1.18x | 451.09 s | 1.19x |
| `hmm_drive_1` | 6 | 147,829 | 1.15x | 6.94 s | 0.56x |
| `hmm_gaussian` | 14 | 263,917 | 1.14x | 18.75 s | 0.05x |
| `radon_county` | 389 | 82,076 | 1.12x | 4.49 s | 1.11x |
| `wells_dist100ars_model` | 3 | 18,997 | 1.12x | 0.62 s | 1.51x |
| `hmm_drive_0` | 6 | 132,850 | 1.11x | 3.65 s | 0.37x |
| `gp_regr` | 3 | 4,698 | 1.11x | 0.23 s | 2.88x |
| `normal_mixture` | 3 | 88,239 | 1.11x | 1.13 s | 1.38x |
| `wells_dist100_model` | 2 | 17,195 | 1.11x | 0.47 s | 1.88x |
| `wells_dae_inter_model` | 7 | 21,310 | 1.09x | 0.55 s | 1.72x |
| `wells_dae_c_model` | 5 | 19,308 | 1.08x | 0.59 s | 1.51x |
| `low_dim_gauss_mix` | 5 | 98,315 | 1.08x | 1.98 s | 1.36x |
| `nn_rbm1bJ10` | 7951 | 185,731 | 1.08x | 456.82 s | 0.92x |
| `wells_interaction_model` | 4 | 20,402 | 1.07x | 0.94 s | 1.34x |
| `low_dim_gauss_mix_collapse` | 5 | 95,373 | 1.07x | 4.45 s | 1.11x |
| `wells_dae_model` | 4 | 20,356 | 1.07x | 0.76 s | 1.33x |
| `dogs_hierarchical` | 2 | 34,053 | 1.07x | 0.68 s | 0.28x |
| `wells_daae_c_model` | 6 | 20,885 | 1.07x | 0.63 s | 1.21x |
| `wells_interaction_c_model` | 4 | 20,272 | 1.07x | 0.49 s | 1.81x |
| `eight_schools_centered` | 10 | 314 | 1.05x | 0.19 s | 3.80x |
| `Mt_model` | 4 | 19,984 | 1.05x | 0.48 s | 1.66x |
| `ldaK2` | 7 | 104,059 | 1.05x | 3.19 s | 1.41x |
| `bones_model` | 13 | 51,501 | 1.02x | 1.31 s | 1.49x |
| `gp_pois_regr` | 13 | 3,935 | 1.01x | 1.47 s | 1.04x |
| `Survey_model` | 1 | 61,578 | 1.01x | 1.14 s | 0.57x |
| `diamonds` | 26 | 31,497 | 1.01x | 48.55 s | 0.94x |
| `Mb_model` | 3 | 49,570 | 0.94x | 1.15 s | 0.37x |
| `dogs_nonhierarchical` | 65 | 40,588 | 0.92x | 2.86 s | 0.68x |
| `Mh_model` | 388 | 38,956 | 0.89x | 2.66 s | 0.78x |
| `grsm_latent_reg_irt` | 408 | 762,133 | 0.79x | 66.92 s | 1.21x |
| `multi_occupancy` | 106 | 58,996 | 0.79x | 7.34 s | 0.77x |
| `one_comp_mm_elim_abs` | 4 | 470,681 | 0.75x | 11.23 s | 0.73x |
| `gpcm_latent_reg_irt` | 530 | 1,337,651 | 0.74x | 161.93 s | 1.24x |
| `iohmm_reg` | 29 | 320,335 | 0.65x | 181.23 s | 0.71x |
| `soil_incubation` | 6 | 60,871 | 0.59x | 12.84 s | 0.55x |
| `lotka_volterra` | 8 | 41,313 | 0.53x | 6.24 s | 0.62x |

120 models; 119 with both gradients; median per-gradient speedup 2.18x; 109/119 at or above CmdStan.

### The models the run could not complete

A missing sampling number is not a missing gradient. These rows sort below the
complete table, and measured gradient ratios still stand.

| model | params | CmdStan ns/grad | grad speedup | what stopped it |
| --- | ---: | ---: | ---: | --- |
| `ldaK5` | 7714 | 5,580,314 | 1.52x | stanli sampling hit the 900 s cap; CmdStan sampling hit the 900 s cap |
| `nn_rbm1bJ100` | 79411 | 434,981,254 | 1.06x | stanli sampling hit the 900 s cap; CmdStan sampling hit the 900 s cap |
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
