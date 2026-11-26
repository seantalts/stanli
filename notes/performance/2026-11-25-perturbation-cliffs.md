# 2026-11-25 · Performance cliffs found by perturbing corpus models

This note records what `tools/perturb/` found when it rewrote corpus
models in ways that a user might, grew their data, and changed data values.
It ranks the causes by the gate in the runtime that produces them. It
proposes no change; each gate lists what a fix would need.

Tree: the runtime of main at 6b00234d1, a Release build with debug switches
and the embedded compiler. The harness is on `perf/perturbation-fuzz`.
CmdStan is the pinned one. The timing ran on a machine that other work was
using (1-minute load 4 to 14). A second timing of one variant per gate
agreed with the first within 8% for every variant (27 variants; the
largest change was 3.26x to 3.02x), so the ratios below are stable, but
none of them is a corpus benchmark.

Evidence, under `data/2026-11-25-perturbation-cliffs/`:

- `variants-results.json.gz`: every variant, both programs, both timings.
- `corpus-scaling.json.gz`, `corpus-datavalues.json.gz`: the size ladders
  and the data-value perturbations of the 402 corpus models.
- `loop-fallback.md`, `lane-declines.md`, `unexplained.md`,
  `size-growth.md`: the four analyses, with the gate, the condition, and
  whether each claim was run or only read. Each has a directory of small
  reproducers. The analyses name helper scripts and work directories that
  are not checked in; the reproducers run with `dump_ops` and `bench_grad`
  as `tools/perturb/README.md` describes.

## Method

A variant is a corpus model with one change. An equivalent variant keeps
the parameters block and must give the base's log density and gradient at
three points to 1e-10 (a constant offset is allowed). The harness then
compares the two prepared programs, times both gradients in stanli, and
times both in CmdStan. A **cliff** is a variant where

```
(stanli variant / stanli base) / (CmdStan variant / CmdStan base) >= 1.3
```

over 24 interleaved pairs. This measure flags more than gates. A variant
that does more work, or a base that is fast through a special case, also
passes it. The tables below sort those out.

Three kinds of change were tried:

- **Source rewrites.** 534 variants of 81 seeds in 30 classes. Each class
  was chosen to step outside one condition that a pass checks.
- **Size.** 333 corpus models have a size variable that `scale.py` can
  scale. Each was swept over a ladder from 1 to 10^5, with points on either
  side of each cap in lowering.
- **Data values.** All 402 models, with duplicate rows, discrete
  covariates, constant columns, another index order or unbalanced groups.
  The source and the sizes stay the same.

Of the 534 source variants, 23 prepare to the same program, 341 change the
program and stay within 1.3x, 63 slow down in both systems, and 107 pass
the cliff measure.

## One correctness bug

A loop that stores the same computed value in every row (`sg[n] = exp(a)`)
was fused into a store of length 1. The log density and gradient were
wrong without any error. Issue #690, fixed in #691.

## Gates, ranked

"Corpus today" counts the corpus models that reach the gate with their own
source and data. The counts come from decline reasons, and several causes
can share one reason, so read them as upper bounds. "Cliffs" counts the variants that pass the cliff measure.
"vs CmdStan" is the worst variant's time against CmdStan on the same
variant. Timings are from the second run.

### Gates that corpus models reach today

| gate | corpus today | cliffs | size of the effect | what a fix needs |
|---|---|---:|---|---|
| The cost model for repeated lanes (`partition.cpp:1025-1095`, the same formula as `reroll_plan.hpp:194-222`) | 19 bases decline on cost today (13 in partition, 10 in reroll). How many of them sit between the measured break-even and the model's threshold was not measured. 14 more models reach the gate only when their data gets repeated values: the 12 radon models, low_dim_gauss_mix and its collapsed form. | data values, not source | It fuses only above about 46% distinct lanes. Measured break-even is 10% to 20%. In the reproducer with 4,000 rows, 1,900 distinct rows fuse to 11 ops and 1,800 give 3,972 ops and run 4.5x slower. | Raise the price of a scalar op (one formula, shared by both passes), or fuse only the distinct lanes and multiply by integer counts. First measure the share of distinct lanes in the 19 bases: only those between about 15% and 46% would change. |
| A container built per row (`reroll.cpp:1008-1024`, `partition.cpp:253-256`, `762-776`, `976-977`) | normal_mixture_k runs at 7,159 ops as written. 39 bases have the reroll decline, 21 the "no lane rule" decline. | 11 | 11 to 21 ops become 2,000 to 15,000. 1.5x to 5.3x. Four variants end up to 2.3x slower than CmdStan. | Treat a row container of width K as one lane value, with row rules for `log_sum_exp`, `log_mix` and concatenation. This is the largest change in this note. |
| The unroll ceiling counts ops before the passes (`lower.cpp:1345`, `lower_stmt.cpp:2482-2484`) | Any model whose row loop lowers to more than 2^20 ops. In the sweep, 19 models switched to `OP_LOOP` at some size. | size | radon at N=149,700 folds to 21 ops. At N=149,900 it is one `OP_LOOP`: 14.4x slower for 0.13% more rows, and 1.5x slower than CmdStan. | Find the fused form without unrolling everything: lower the body once over whole columns, or prove the lanes on a prefix and extend the strides. Reproduce with `scale.py base.stan base.json --set N=149700` and `N=149900` on radon_variable_intercept_slope_noncentered. |
| Preparation: a loop bound is evaluated again on every iteration (`mir_interp.hpp:268`, `:333`) | 2pl_latent_reg_irt, gpcm_latent_reg_irt | size | 28 s of preparation at K=1000, growing as K^1.9. | Evaluate the bound once. |
| Preparation: a constant write keeps a copy of the whole container (`lower_expr.cpp:110`, `:128`) | hbayesdm_prl_rp, hbayesdm_dd_hyperbolic | size | 4.8 GB and 3.3 GB at 63 subjects. Memory grows as the square of the container. | Update in place, or keep one copy per container. |
| Preparation: reroll counts a run of stores again from every start (`reroll.cpp:1795`) | hbayesdm_prl_rp | size | Scan steps grow 4x per doubling, and nothing is fused. | Remember where a run was refused. |
| `write_array` writes outputs one scalar at a time (`reroll.cpp:755`, `:928-937`) | 23 models (brms group effects, ldaK, probit_multi_good) | size | One op per element. The cost is per draw, not per gradient. | Let a run of indexed moves start a lane group. |

### Gates that only variants reach, where stanli ends slower than CmdStan

| gate | cliffs | size of the effect | what a fix needs |
|---|---:|---|---|
| Region map refuses a body that assigns `mu[n]` when `mu` is declared outside the loop, or that uses `continue` (`lower_region_map.cpp:367-375`). This is also why the loop above the unroll ceiling cannot use the region map. | 14 (the cogmod models with transforms moved into the row loop) | 2.3x to 4.8x. 1.3x to 1.9x slower than CmdStan. One corpus base has this refusal. | Treat `x[n]` as private to the row when every access uses the loop variable and nothing reads `x` afterwards. Compile `continue` as a forward jump. |
| An inner loop with a bound per subject has no generated backward (`adjoint.cpp:62-64`, inferred) | 13 (cogmod trials nested in subjects) | One island per row, 16 ops to 1,055 or more. 2.3x to 3.9x. Up to 1.7x slower than CmdStan. | Flatten the nest into one map over (subject, trial) pairs. |
| A read of another element stops forwarding (`inplace.cpp:288-304`) | 3 (radon with a dead `d[n] = mu[n] - mu[1]`) | 16x to 20x. 1.7x slower than CmdStan. | Remove unread ops before forwarding. |
| `Y[d][n]` copies the whole row (`mir_prog_expr.cpp:314-322`) | 1 | The body passes the cap of 16,384 instructions. 17x. Preparation goes from 48 ms to 4.6 s, most of it the unroll up to the ceiling. | Use a view or one indexed read. |
| A row loop counts as carrying a value when an inner loop does (`lower_structured_loop.cpp:2822-2833`) | 1 (diamonds with an inner sum over columns) | 2.8x, and 2.8x slower than CmdStan. A `real acc` declared in the row body does the same: 18.5x at 8,000 rows in the reproducer. | Ignore a carry that stays inside one row. |
| A hoisted value is not an invariant base, and CSE does not merge active transposes (`partition.cpp:806-807`, `900-910`; `cse.cpp:240-245`) | 1 (radon, `(abt')[county[n]]'`) | 21 ops to 52,825. 65x, and 7.0x slower than CmdStan. | Count hoisted outputs as invariant bases. Merge ops whose backward only copies or permutes. |
| `log_mix` on a vector runs through the Stan Math wrapper, on a nested tape in forward and again in backward (`library_fns.cpp:51-66`) | 2, and part of the row-container group | About 420 ns per call against 36 ns for `log_sum_exp`. 1.9x slower than CmdStan. | A native kernel, or lower it to `log_sum_exp(log(theta) + lambda)`. |
| `hmm_marginal` is evaluated in forward and again in backward (`library_fns.cpp:247`) | 4 | stanli gets up to 1.7x slower where CmdStan gets 1.5x faster. 1.6x slower than CmdStan. | Evaluate once and keep the input gradients. Needs the ULP check. |

### Gates that only variants reach, where stanli stays ahead of CmdStan

| gate | cliffs | size of the effect | what a fix needs |
|---|---:|---|---|
| The gather backward finds no runs when indices alternate between two slots (`elementwise.cpp:323-345`) | 9 (every 2-vector spelling of the radon group effects) | Same op count, 1.6x. Still 6.5x faster than CmdStan. | Find runs at a fixed period, or lower a small `GROUP_DOT` as separate gathers. |
| A standardized residual `(y - mu) / sigma` fuses, but to a slower kernel route (no lane gate) | 1 (rats_model) | 14 ops to 19. 2.7x where CmdStan slows 1.6x. Still 4.5x faster than CmdStan. | A summed density form for a density minus a constant. |
| Lanes mix a scalar per lane with a row of width W (`reroll.cpp:1066-1078`, `partition.cpp:762-776`) | 5 (radon groups walked with `segment` or a nested loop) | 13 ops to 10,000 through 34,000. 3x to 4.5x. | Vector constants in partition, and a lane scalar repeated across its row. |
| A run of stores must step by a constant positive stride (`reroll.cpp:1268-1281`, `partition.cpp:847-858`) | 5 (a loop that runs downward or in sorted order) | 1.6x to 4.2x. | Accept a negative stride by reversing the lane order. A general permutation needs a gather through its inverse. |
| A statement per row for each of D outcomes goes to the region map (`partition.cpp`: lane widths do not agree) | 5 (radon with 16 to 32 outcomes per row) | 5.7x to 6.6x more than CmdStan for the same extra work. Still 2x faster than CmdStan. | Partition lanes of width D. |
| A row reads back the element that its own store writes (`inplace.cpp:288-301`, `reroll.cpp:1158`, `partition.cpp:590-602`) | 3 | 21 ops to 1,905. 3.1x. | Forward reads of elements that were never written. |
| Reroll takes the shortest period first (`reroll.cpp:939-948`) | 3 (a row-by-row matrix fill; censored rows at every 10th row) | 1.5x. | Strided stores in the island vocabulary. Merge groups that share a template. |
| `normal_lccdf` costs more than the same math through `normal_lcdf` (`densities_cdf_b.cpp:16-30`, cause inferred) | 1 | 98 ns against 61 ns per call. | Negate the inputs without copying the context. |

### Not traced

| variant | effect | what is known |
|---|---|---|
| arK with `mu` stored in a vector and then one vectorized `~` (`C23__mu_vector_then_sample`) | 7.2x slower where CmdStan gets 1.2x faster. Level with CmdStan afterwards. | 21 ops become 9 with an island. Partition has used its split budget and reroll says that lanes read different slots. No analysis looked at it. |
| stochvol with a scalar AR recurrence (`C17__scalar_ar`) | 1.5x | 43 ops become 11. Not analysed. |

### Slow by an earlier decision

These pass the cliff measure and are listed so that nobody finds them
again. Each follows from a decision that is already recorded.

| group | cliffs | why |
|---|---:|---|
| A branch on a parameter inside a row loop (a function with an early return; a `reject` check on `sigma`) | 8 | Parameter conditionals may stay slow. The region map takes the loop and runs about 15 instructions per row. A `reject` that is the same in every iteration also blocks the fold of the loop (`lower_stmt.cpp:1294-1297`). |
| One active scalar read at two positions of a lane (a mixture with a shared `sigma`) | 3 | The gate protects the accuracy of the shared adjoint. Fast mode turns it off: 5,207 ops become 13. |
| Matern 3/2 covariance | 4 | Only the exponentiated quadratic has a closed-form backward (`matrix_fns.cpp:288`). `OPTIMIZATIONS.md` gives the reason. 2.95 µs against 0.12 µs per gradient. |

### Not cliffs

| group | flagged | why |
|---|---:|---|
| ch12_m12_4 with a vectorized or probit likelihood | 4 | The base is about 700x faster than CmdStan because its 9,930 scalar `ordered_logistic` terms are identical and CSE merges them into 7. A vector statement gives CSE nothing to merge, and the variant runs level with CmdStan. The categorical spelling does not merge because `OP_CATEGORICAL` counts as effectful (`optable.hpp:986`, checked by reading). `unexplained.md` gives a second reason, that CSE keys a data slot by its id; that one would apply only if the first did not. |
| blr with 200 columns, arK with 40 lags | 4 | The variant does 7x to 40x the work. CmdStan's ratio is smaller only because its fixed cost per gradient dominates the small base. stanli is 1.4x to 6.4x faster on every one. |

## What held

No variant in these classes passed the measure: `~` against `target +=`
and `_lupdf`, a `prior_only` flag, `reduce_sum` at several grain sizes, a
named temporary, the non-centered spellings, a GLM written in other forms,
branches on data with many arms, generated quantities per row, and a loop
against a vectorized `~`.

## Size: what the 60 "no decline" models were

The first cause table had 60 models whose op count grew with no decline
reported. That group was mostly an error in the table:

- 25 were there because a decline went away. The table now files those
  separately.
- 23 are the scalar `write_array` outputs above.
- 3 have an `_rng` loop in generated quantities, which reroll does not
  match and does not report.
- 2 have a second loop level that no pass scans (grsm, kronecker_gp).
- 7 have only preparation findings.

The decline report itself had holes, and the harness now covers two of
them: refusals from the region map and from structured loops are read as
declines, and a decline that repeats a reason that the base already has is
counted. Three decisions still print nothing: forwarding that stops at a
read, the fold of a loop that is the same in every iteration, and the
choice of the region map for a branch on a parameter.

## Which fixes reach the corpus

The acceptance bar for a pass change is the corpus benchmark on the models
whose program changes. A gate that only variants reach changes no corpus
program, so it cannot pass that bar on its own. Whether to fix those is a
separate decision. By reach:

1. **The cost model for repeated lanes: measured on 2026-11-26, and
   left alone.** Forcing fusion on the 19 bases that decline on cost
   slows four of them (ch12_m12_3 11x, ch12_m12_3_alt 2.5x,
   multi_occupancy 1.4x to 1.7x, dogs_hierarchical 1.3x), speeds up two
   (dogs 1.28x, multinma_ndmm_mspline 1.07x) and leaves the rest within
   3%. No single price for a scalar op separates them: the shapes need
   prices between 18 and 55. A count of distinct lanes per position does
   not separate them either, because keeping ch12_m12_3 scalar needs a
   price of at most 8.4 and fusing the radon shape at 20% distinct needs
   at least 26.5. The repeats that matter sit across reroll groups, which
   a count per group cannot see. So the declines in the corpus are mostly
   right, and the gate still costs the 14 models that reach it through
   repeated data values.
2. **The fused form above the unroll ceiling: fixed on 2026-11-26 in
   stanc3.** The fork's `vectorize_loops` now turns a loop whose statements
   all read and write the same element `mu[n]` into vector statements, so
   the row loop never reaches the unroll. radon at N=149,900 and at
   N=10^6 gets the same 21 ops as a full unroll, and preparation at 10^6
   takes 0.37 s and 334 MB where the full unroll took 5.9 s and 3.2 GB. A
   loop that the pass does not take still meets the ceiling as before.
3. **The three preparation causes.** Each is local and has a reproducer.
4. **The row container.** The largest change, and the one that covers the
   most variants (11) and one corpus model as written.
5. **The region map's refusal of `mu[n]`.** It covers 14 variants, and it
   is also what stops the loop above the ceiling from taking the region
   map.

## Limits of this run

- Seven corpus bases fail the validity check at the harness's random
  points (for example `sir` and `kronecker_gp`), so a variant of one of
  them could not be checked.
- `scale.py` refuses 174 size variables with a compound dimension, 65
  models with no integer size, and 17 with ragged groups.
- The count of corpus models per gate comes from decline reasons, so the
  gates that print nothing have no count.
- Small ratios (under 1.5x) on bases under 2 µs are within the noise of
  this machine and should be timed again before anyone acts on them.
