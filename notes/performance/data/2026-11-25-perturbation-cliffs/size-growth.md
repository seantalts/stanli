# Size growth: root causes

Worktree `the perf/perturbation-fuzz worktree`, prebuilt `build-rel`, evidence in
`corpus-sweep/` (not checked in). No source was edited and nothing was rebuilt.
Every claim is marked **[ran]** (I ran a command and saw the result) or
**[read]** (I only read the code or the sweep JSON).

Reproducers:

- `rootcause/size-growth/A/repro_A.sh` (A, a few seconds)
- `rootcause/size-growth/B/repro_ceiling.sh` (B, about 3 s)
- `rootcause/size-growth/C/repro_C.sh` (C, under 30 s, under 1 GB)
- `rootcause/size-growth/list_none.py` lists the 60 models of question A.
- `rootcause/size-growth/classify_opsgrow.py` dumps each op-growth finding at
  sizes 8 and 16 and prints the opcodes that grow. Its output is in
  `A/opsgrow_classes.json`.
- `rootcause/size-growth/mir_index.txt` maps each seed to its cached MIR.

## A. "No new decline or lowering switch" (60 models)

### First, the bucket is mostly made by causes.py itself

`tools/perturb/causes.py:44-46` takes the causes of a `program_changes_shape`
finding only from the entries that start with `+`. A finding whose entries all
start with `-` (a decline went away, or ISLAND went away) gets the cause
"no new decline". **[read]** Each model's findings fall into these classes
**[ran: list_none.py]**:

| finding in the bucket | models |
|---|---:|
| `program_changes_shape` whose entries are all removals | 28 models, 33 findings (25 models have nothing else in the bucket) |
| `ops_grow` on write_array | 27 |
| `ops_grow` on log_prob | 2 |
| `prep_superlinear` | 9 |

The 28 models have 33 removal-only findings. I compared the log_prob plus
write_array op counts at the two sizes of each one in scaling.json **[ran]**:

| ops between the two sizes | findings |
|---|---:|
| fall | 21 |
| stay the same | 8 |
| grow, but more slowly than the size | 4 |

The 4 that grow are Mtbh_model, cm_betadiscrete, examplemodels_cjs_cov_raneff
and i320_mi_nhanes. In these, declines that are present at both sizes explain
the growth, and causes.py does not look at those declines for a shape finding.

Neither example that causes.md names is a model whose ops grow with no decline:

- **Mtbh_model M 9 -> 31:** the op count rises from 228 to 633 while one
  partition decline goes away. The declines present at both sizes explain the
  growth. Its main op growth (`ops_grow M 8 -> 10000`) is filed under
  write_array declines. **[ran: scaling.json]**
- **2pl_latent_reg_irt K 33 -> 100:** this is a `prep_superlinear` finding,
  and its op count stays at 17 for every K. The cause is in the interpreter
  (C1 below), so no lane report could ever show it. **[ran]**
- **The radon family:** 11 models have the removal-only step N 1000 -> 10000.
  In radon_variable_slope_centered, for example, the program has 13 ops at
  N=100, 1,249 ops (with 5 declines) at N=1000, and 13 ops again at N=10,000.
  causes.py files the worse step, 100 -> 1000, under its new declines. It files
  the step back, 1000 -> 10000, under "no new decline". **[ran: scaling.json]**

**What should change:** causes.py should file a finding that only removes
entries in its own group (or drop it when ops do not grow). When ops do grow, it
should give the declines present at both sizes as the cause. It should keep "no
new decline" for growth that nothing explains.

### The 29 op-growth findings (28 models): which ops grow

`classify_opsgrow.py` dumps every finding at sizes 8 and 16 with
`STANLI_LANE_REPORT=1` **[ran]**:

| class | models | which ops grow | LANES records for that graph |
|---|---:|---|---|
| A1. write_array writes its outputs one scalar at a time | 23 | only `INDEX` (1 per output element) | 0 |
| A2. generated quantities loop that calls an `_rng` | 3 | `RNG`, the loop's density and arithmetic, element stores | 0 |
| A3. second level of a nested loop, after one level fused | 2 | one fused op per outer iteration | 0 |

The models in each class:

- **A1:** i319_gauss_re, i319_negbin_re, i319_pois_re, i319_pois_re2,
  i320_gp_expquad, i320_gp_matern32, s2_categorical_re, s2_dist_sigma_re,
  s2_gp_approx, s2_gp_by_approx, s2_gr_by, s2_gr_student, s2_mm,
  s2_mm_weights, sw_gp, sw_re_bern, sw_re_gauss, sw_re_negbin, sw_re_pois, ldaK2,
  ldaK5, examplemodels_probit_multi_good, ch14_m14_7.
- **A2:** aalto_poisson_simple, dogs_nonhierarchical, hbayesdm_dd_hyperbolic.
- **A3:** grsm_latent_reg_irt (log_prob, I) and kronecker_gp (log_prob and
  write_array, n2).

Over the 60: 25 are removals only, 23 are A1, 3 are A2 and 2 are A3. The
other 7 have only prep findings:

- 2pl_latent_reg_irt and gpcm_latent_reg_irt: cause C1.
- gp_regr, gp_pois_regr and examplemodels_gp_fit_multi_output: an N x N
  covariance matrix, so their memory grows as N^2 by design. **[read]**
- radon_partially_pooled_centered and radon_partially_pooled_noncentered: RSS
  exponent 1.2, close to the threshold.

**A1. write_array writes its outputs one scalar at a time.**
Example: i319_gauss_re M_1=3, where write_array has 192 ops and 178 of them are
`INDEX` **[ran]**:

- The INDEX ops read `z_1` (an `array[M_1] vector[N_1]` parameter) at
  idata 0, 59, 118, 1, 60, 119, ..., which is Stan's first-index-fastest output
  order.
- Lowering makes one output view per `FnWriteParam`
  (`runtime/src/lower_stmt.cpp:2166`, **[read]**).
- I infer from the op order that stanc emits a nested loop of scalar writes for
  an array of vectors or a matrix. The loop unrolls to one INDEX and one view
  per element. **[inferred]**

I checked six more models from four collections at size 16 with
`A/index_feeds.py` **[ran]**. In each, almost every INDEX output goes only to
an output view, and no other op reads it:

| model | write_array ops | INDEX ops | INDEX outputs that another op reads |
|---|---:|---:|---:|
| ldaK5 | 387 | 385 | 0 |
| examplemodels_probit_multi_good | 8,004 | 8,000 | 0 |
| ch14_m14_7 | 42 | 32 | 0 |
| s2_gr_by | 95 | 80 | 0 |
| i320_gp_expquad | 31 | 18 | 2 |
| i319_pois_re2 | 3,853 | 3,837 | 2 |

Reroll never tries these ops:

- Its candidate predicate, `is_candidate_op` (`runtime/src/reroll.cpp:755-760`),
  accepts only densities, element or row stores, and widenable ops whose output
  is a target term. An INDEX whose result is an output view is none of these.
- A period is tried only when the first candidate op after the start lies
  inside the first lane (comment at `reroll.cpp:875-878`, code at
  `reroll.cpp:928-937`). In i319 the only candidates are the three
  STUDENT_T_LPDF ops at the end of the graph. So no period that starts in the
  INDEX run is tried, and nothing is recorded.
- **[ran]** For i319 at M_1=3, write_array reroll reports `candidate_steps=388`
  for 192 ops. The scan ran over the graph, so this is not the early return
  at `reroll.cpp:777` (which happens when the graph has no candidate at all).
- Partition is off for write_array (`kWriteArrayPasses`,
  `runtime/src/lower_internal.hpp:2959`). **[read]**

The op count is tied to the number of output scalars by the way stanli
represents views. It costs one dispatch per element per draw, not per
gradient. So I expect little effect on sampling speed **[inferred]**, but it
does grow the program and the prep time. **Fix:**

- Lower a `FnWriteParam` loop nest over one container as one gather or
  transpose into a container view.
- Or give reroll an anchor for runs of INDEX ops whose outputs are consecutive
  views.

**A2. A loop that calls an `_rng` function.**
Example: aalto_poisson_simple. The loop body `y_rep[n] = poisson_rng(lambda);
log_lik[n] = poisson_lpmf(...)` lowers to RNG, SET_INDEX, POISSON_LPMF and
SET_INDEX per row. The POISSON_LPMF and the stores are candidates, and the
signatures repeat, but `ops_match` refuses any effectful opcode
(`reroll.cpp:145`). `is_effectful_op` (`runtime/include/stanli/optable.hpp:978`)
lists OP_RNG, OP_CATEGORICAL and the CHECK ops. So no lane
pair matches. The lane count stays below `kMinLanes` = 4 and the pass moves on
at `reroll.cpp:999-1002` without recording a decline. **[ran: 0 LANES records;
read: the code path]**

This is a hole in the decline report. A lane group that fails to match its
first 4 lanes is never reported, whatever the reason. **Fix:**

- Record a decline (for example "a lane holds an op with an effect") when the
  lane count stops below 4 at a candidate.
- To fuse such loops: OP_RNG with the same arguments in every lane could widen
  to one vector draw, if the RNG stream order is kept the same (draws in lane
  order). That needs a proof that the order of draws is unchanged.

**A3. Second-level lanes are made by a pass and never scanned again.**

- grsm_latent_reg_irt I=16: partition built one CATEGORICAL_LOGIT_GLM_LPMF per
  item, each over 392 persons. Each item's template is INDEX alpha[i], INDEX
  beta[i], MUL and the GLM, and it repeats I times. **[ran]**
- kronecker_gp n2=32: reroll turned the inner loop of `calculate_eigenvalues`
  into one `SLICE, MUL, ADD, SET_SLICE_STRIDED_INPLACE` row per outer
  iteration, and those 4-op rows repeat n2 times. **[ran]**

Reroll runs once and scans only the original graph. Its own output and
partition's output are never scanned again: `lower.cpp:1007-1065` runs reroll,
then partition, then CSE, with no second reroll. So no pass looks at these
runs, and nothing is reported. **[read]** The cost is a few ops per group,
which is small. **Fix:** a second reroll after partition, limited to ops that
the first round emitted.

### Where the decline report has holes (all [read])

1. A run whose first lane holds no candidate op is never tried
   (`reroll.cpp:928-937`), and a graph with no candidate at all returns at
   `reroll.cpp:777`. Neither case leaves a record (A1).
2. `reroll.cpp:999-1002`: fewer than 4 lanes match, so reroll moves on with no
   record (A2, and any effectful op).
3. Partition does not run on write_array (`lower_internal.hpp:2959`), so it can
   never record anything for that graph.
4. `prep_superlinear` findings are given causes from the declines and lowering
   opcodes at the largest size (`causes.py:47-54`). A cost that is spent before
   lowering (C1) or in lowering itself (C2) can never show up as a cause.
5. The `--write-array` call to dump_ops in `tools/perturb/measure.py` runs
   without `STANLI_LANE_REPORT`. The write_array declines in the sweep still
   appear, because the plain dump_ops run prepares both graphs and prints
   records for both. **[ran: Mb_model T=33 printed 4,438 write_array reroll
   records in the plain run.]** So this is not a hole, but it looks like one.

## B. The unroll ceiling and the program budgets

A forked agent did this part. I reran its reproducer, `B/repro_ceiling.sh`, and
got the same results:

- N=10,000 fuses to 21 ops.
- With the ceiling set to 50,000, the region map refuses ("assigns mu declared
  outside the body") and the program becomes one LOOP op, 18 ops in all.
- N=149,700 lowers to 1,047,914 ops and folds to 21.
- N=149,900 becomes `budget_log_prob` with 18 ops.

I also read the code at `lower.cpp:1345`, `lower_stmt.cpp:2482-2484` and the
region-map refusal, which is at `lower_region_map.cpp:374`. I did not check the
fork's other line references.

**The gates [read]:**

- **Unroll ceiling.**
  - `kUnrollCeilingOps = 1<<20` is set at `runtime/src/lower.cpp:1345`. The
    `STANLI_UNROLL_CEILING` override is read at `lower.cpp:1370`.
  - The check is at `runtime/src/lower_stmt.cpp:2482-2484`, after each
    iteration of an outermost loop. It compares the raw ops that lowering
    emitted for that loop (`g.ops.size() - ops_before`) with 2^20, before any
    pass runs.
  - It applies only to "budgeted" loops (`lower_stmt.cpp:2460-2468`): outermost,
    no effects, no parameter-dependent control, and no int assigned that is
    used later.
  - `compile_model` catches the throw (`lower.cpp:1833-1846`) and lowers the
    whole model again as `budget_log_prob` through `lower_compact`
    (`lower.cpp:1462`).
- **8 MiB program budget** (`lower.cpp:1341`, used at `lower.cpp:1848-1851`
  through `loops_over_budget`, `lower.cpp:1380`). It compares
  `Executor::program_bytes` (`executor.cpp:915`) with 8 MiB, and it is
  measured after the passes. It charges each loop only for the post-pass ops
  that write slots the loop's unroll created.
- **32 MiB loop budget** (`lower.cpp:1342`, `op_loop = first_bytes >
  loop_budget` at `lower.cpp:1849`). This is also measured after the passes.
  Below it, OP_LOOP is not allowed as the compact form.
- **write_array** uses the same three limits through
  `WriteArrayLowering::keep_under_budget` (`lower.cpp:1609-1630`). Its only
  compact form is OP_LOOP.

**Answer to "before or after the passes":** the ceiling counts the graph
before the passes. Both budgets count it after. A loop that reroll folds to 21
ops never goes over a budget, but it still goes over the ceiling.

**Radon arithmetic [ran by the fork, on radon_variable_intercept_slope_noncentered]:**

- The loop lowers to 7 ops per row, plus 14 ops outside it.
- At N=149,700 lowering emits 1,047,914 ops and the passes fold them to 21.
- At N=149,900, 7 x 149,900 = 1,049,300 is more than 2^20 = 1,048,576. The
  model becomes `budget_log_prob` with one LOOP op, 18 ops in all.
- The predicted crossing, N of about 149,795, falls inside the pilot's step
  145,304 -> 154,292.
- The thrown-away first unroll takes about 600 ms of the 610 ms compile.

**Why the fused form is lost above the ceiling.**

- Reroll and partition find lanes by comparing iterations that lowering has
  already emitted. The 21-op form (GATHER, vector MUL and ADD, NORMAL_LPDF)
  exists only after all N copies exist.
- Above the ceiling, lowering first tries the region map
  (`lower_stmt.cpp:2396-2413`). The region map refuses radon with "assigns mu
  declared outside the body" (`lower_region_map.cpp:374`, seen with
  `STANLI_REGION_MAP_DIAGNOSTICS=1`), even though `mu` is only written and read
  at index n. **[ran by the fork]**
- Lowering then falls back to OP_LOOP, which lowers the body once into a
  structured program. The graph passes see only the ops outside the loop, so
  nothing is left for them to fuse.
- i319_gauss_re's `mu[n] += ...` fills a vector that is read after the loop,
  so the region map has no form for it at all. **[read]**

**What it would take to find the fused form without unrolling everything
[inferred]:**

1. Lower the body once and treat data indexed by the loop variable, including
   gathers through data index arrays, as whole columns. Then emit the vector
   ops that reroll would have produced.
   - `notes/performance/audit-2026-11-20-explicit-loop-lowering.md` already
     proposes this.
   - Preconditions: no parameter-dependent control, no state carried between
     iterations, and the same op shape in every iteration.
   - The value folding that lowering does row by row (`fold_const` and the
     observations in C2) would have to stop or be proven uniform.
2. Unroll a prefix of K iterations, prove the lanes on it with reroll, then
   extend the strides and gather indices to all N.
   - Every remaining iteration still has to be shown to lower to the same
     template. That check evaluates the index expressions only and emits no
     ops.
   - The prefix retry in `reroll_plan.hpp:225-230` works only inside one graph,
     so it does not do this today.
3. A narrower fix for radon: let the region map treat a container written at
   `[loopvar]` as local to the body when nothing reads it after the loop. This
   gives the region-map form, not the 21-op form, and does not help i319.
4. In any case, stop the unroll early, or predict the folded size from the
   first iterations. Today the first attempt spends about 600 ms that are then
   thrown away.

The fork also noticed two things:

- Between the 8 MiB budget and the ceiling, a loop that the region map refuses
  is never made compact, because OP_LOOP is off below 32 MiB.
- irt_1pl_hier does not fuse in the current sweep, so the fork did not use it.

## C. Preparation that grows faster than the size

The sweep has 43 `prep_superlinear` findings over 29 models. I picked three
with different shapes and found three separate causes, each with a small
reproducer (`C/repro_C.sh`).

### C1. The interpreter copies a whole matrix for `rows(W)` in a loop bound

Models: 2pl_latent_reg_irt and gpcm_latent_reg_irt (K = 1000 takes 28 s).
grsm_latent_reg_irt has the same function.

- **[ran]** With `STANLI_PROFILE_PREP=1`, all the time is in `bind_data`, which
  runs transformed data: 115 ms at K=50 and 385 ms at K=100. Lowering and the
  passes take under 1 ms, and there are 17 ops.
- **[ran]** `sample` at K=300 (3.99 s): 2,192 of 2,192 samples are in
  `MirInterp::call_udf` (`obtain_adjustments`), in the
  `for (j in 1 : rows(W))` loop. 1,985 of them are in
  `DataMap::Entry::Entry(const&)`, a memmove, called from `eval(Var)` inside
  `eval_fun` inside `as_int`.

The cause **[read]**:

- The For loop evaluates its upper bound again on every iteration, which is
  what Stan does: `runtime/include/stanli/mir_interp.hpp:333`.
- `eval` of a variable returns a copy of the whole value: `mir_interp.hpp:268`,
  `if (en) return *en;`.
- `rows(W)` goes through `eval_fun`, which evaluates its arguments by value. So
  each inner iteration copies all J x K values of W, and the function costs
  O((J*K)^2).
- `eval_indexed` (`mir_interp.hpp:2078`) already avoids this copy for `W[j, k]`
  (its comment cites the same problem), but shape functions do not.

**Reproducer [ran]:** `C/repro_rows/rows_bound.stan` against `plain_bound.stan`
(the same loop with `1:J`), J=500. Prep time in seconds:

| K | `rows(W)` bound | `J` bound |
|---:|---:|---:|
| 50 | 0.11 | 0.022 |
| 100 | 0.39 | 0.042 |
| 200 | 1.46 | 0.081 |

So the `rows(W)` bound grows as K^1.9, and the `J` bound grows linearly.

**Fix:** evaluate `rows`, `cols`, `size`, `num_elements` and `dims` of a named
variable from a pointer to it, without copying it. Or return a const reference
from `eval` for a plain variable.

### C2. Lowering copies a whole known container at every element write

Models: hbayesdm_prl_rp (4.8 GB at N=63) and hbayesdm_dd_hyperbolic (3.3 GB).

- **[ran]** With `STANLI_NO_REROLL=1`, peak RSS stays the same:
  - prl_rp: 99, 323 and 1,177 MB at N = 8, 16 and 32;
  - dd_hyperbolic: 112 and 364 MB at N = 16 and 32.
  So the memory is not in reroll.
- **[ran]** `STANLI_PROFILE_PREP` puts the rest of the growth in the
  write_array `stage=lower`: 31, 99 and 318 ms for prl_rp.
- **[ran]** `sample` (prl_rp N=48 with reroll off, 2.9 GB; dd N=56, 1.06 GB):
  - The heaviest path is `run_write_array -> lower_simple_stmt ->
    propagate_int_update -> observed_positions -> graph_container_order`.
  - The top of the stack is in `graph_container_order<double/long long>`,
    `bzero` and `memmove`.
  - The destructor of `Lowering::observations`, a hash map from
    `ObservationKey` to `DataMap::Entry`, also shows up.

The cause **[read]**:

- Lowering keeps the known value of each container while it lowers.
  `Lowering::propagate_int_update` (`runtime/src/lower_expr.cpp:99-128`)
  handles an element write of a known value into a known container:
  - It copies the whole container entry (`DataMap::Entry en = *be;`,
    `lower_expr.cpp:110`).
  - It builds two position vectors as long as the container
    (`observed_positions`, `lower_expr.cpp:82-93`).
  - It stores the copy under the new slot (`observe(out_v, ...)`,
    `lower_expr.cpp:128`).
- Each element write makes a new slot, and every copy stays in `observations`
  until lowering ends. A loop of N*T writes into an N x T array therefore keeps
  (N*T)^2 values in memory and does O((N*T)^2) work.
- Both models start their generated quantities with
  `for (i) for (t) y_pred[i, t] = -1;`, and prl_rp also does this for ev_c,
  ev_nc and pe.

**Reproducer [ran]:** `C/repro_observe/init_array.stan` (a generated
quantities block whose only statement is `a[i, t] = 0` over `array[N, 100]
real`). Peak RSS is 33, 110 and 410 MB at N = 10, 20 and 40, which is about
24 bytes per element squared. The program has 0 ops, so the whole cost is this
bookkeeping.

**Fix:**

- Do not keep a full copy per slot. Share the entry and record the writes as a
  delta, or keep one mutable entry for an update chain that has only one user.
- Or stop observing once a container passes a size limit. That limit would
  need a check that the overflow proofs that read these observations still
  hold.

### C3. Reroll counts a long run of matching stores again from every start

Model: hbayesdm_prl_rp. This is its time cost, separate from the memory in C2.

- **[ran]** The write_array `stage=reroll` takes 228 ms at N=8 and 844 ms at
  N=16. `lane_steps` grows from 34.7 M to 138.5 M (4.0x when N doubles), and
  `regions=0`.
- **[ran]** `STANLI_LANE_REPORT` prints 21,563 write_array declines at N=16.
  All of them are on `OP_SET_INDEX_INPLACE` with "lanes read different
  non-constant slots".
- At period 1 there is one decline at each start 33, 34, 35, ..., and the lane
  count drops by one each time (6,396, 6,395, ...). The sum of lanes x period
  over all declines is 69 M, which is about `lane_steps`.
- The run is the init loop: 4 stores per iteration into 4 different vectors.

The cause:

- At period 1 every store matches the next one, because the signature and
  `ops_match` (`reroll.cpp:129-160`) let idata advance and do not compare the
  destination. So the lane count at `reroll.cpp:983-997` reaches the end of the
  run.
- Classification then fails at an early lane, and the attempt loop shrinks
  `Luse` to 1-3 lanes, which is below `kMinLanes` = 4.
- With `Luse > 0`, the soft-failure path (`reroll.cpp:1795-1797`) sets
  `retry_at = i + P * Luse`. The next start is then 1-3 lanes later and counts
  to the end of the run again. That is O(run^2 / P) for each of the up to 40
  periods tried (`kShortPeriod` = 32 plus `kLongPeriods` = 8).
- **[ran: the records; inferred: the code path from those records and the
  code]**

**Reproducer [ran]:** `C/repro_reroll/four_arrays_live.stan` (4 init stores
per iteration, then a parameter-dependent write so that the stores stay in the
graph). `lane_steps` is 1.58 M, 6.20 M and 24.5 M at N = 4, 8 and 16 (T=100),
and reroll takes 12, 43 and 157 ms, with `regions=0`.

**Fix:**

- Treat a failure that leaves fewer than `kMinLanes` lanes as a hard failure,
  so the run is skipped.
- Or remember for each period that the run up to `run_end` has already been
  counted, and reuse that count.
- Or count lanes in blocks only as far as classification can use them.
- `ops_match` could also refuse element stores into different destination
  vectors at P=1, which would end these runs at once.

### The other prep findings

A grep of the sources (only a heuristic, **not run per model**) finds more
models that may share these causes:

- **C1** (a loop bound of the form `rows(...)` or `size(...)`): 7 of the 29
  models, including beeguts_guts_sd, both ecoensemble_hierarchical models and
  examplemodels_ms.
- **C2** (a constant written into a 2-D array in generated quantities): 6
  models, including hbayesdm_gng_m4, hbayesdm_igt_pvl_delta and hmm_gaussian.

I did not profile these.
