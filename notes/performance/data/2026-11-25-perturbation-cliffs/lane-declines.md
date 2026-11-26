# Why reroll and partition leave per-row lanes scalar (perturbation cliffs)

Tree: the perf/perturbation-fuzz worktree, prebuilt `build-rel` tools,
compiled with the embedded compiler and `--vectorize-loops on` as the harness
does. No source was changed and nothing was rebuilt.

Each claim is tagged **[ran]** (I ran a tool and read its output) or
**[read]** (I inferred it from the source). The timings are light: single
`bench_grad --timed` runs of 0.6 s per point. Other timing work was running,
and the load average was between 4.7 and 10.5.

Reproducers are in `rootcause/lane-declines/`. Each one prints the same
decline reason and opcode as the full variant does, unless its section says
otherwise. To run one:

```
P=deps/stanc3/embed/cb6f66058ddc0863/stanli-vectorize-probe
$P --vectorize-loops on --output r.sexp r1_row_container.stan
STANLI_LANE_REPORT=1 build-rel/dump_ops r.sexp mix12.json 1000000000
```

`STANLI_DUMP_PASSES=DIR` writes the graph after every stage. The graph that
reroll sees is `DIR/05-log_prob-constfold.txt`, and the graph that partition
sees is `DIR/07-log_prob-post_reroll_inplace.txt`.

## Where the brief's description did not hold

- rats_model/C1__std_residual: partition fuses it (14 to 19 ops). Its 2.6x
  slowdown comes from the fused kernel route and not from a decline
  (section 2).
- The C13 variants other than transpose_row: they have no lane decline.
  Partition fuses them through a CONCAT2, gathers and GROUP_DOT that cost
  1.6x (section 4b).
- The 431-to-432 radon flip: a 432nd row that is *not* a duplicate flips it
  too. The data sits on the cost threshold (section 5). dugongs and grsm
  reach other gates than the cost model.
- ch12 C14__categorical_loop: `dump_ops` does print a partition decline
  (`lane widths do not agree`, OP_CATEGORICAL), although results.json
  lists no new decline for it (section 7).
- Mth T3 is fine because island carving compiles its scalar fill. No lane
  pass fuses it (section 6b).

## How to read a decline record

- `a plain op reads different slots in the first lanes` (kRigidInputs)
  names the lane's *first* op, not the op that caused it
  (reroll.cpp:1785 records `op_at(0, 0)`). The real gate is
  `rigid_diverges` at reroll.cpp:1008-1024. It looks for an op that is not
  an INDEX, CONCAT2, DOT, element store, row read or store, density or
  widenable op, and that reads a different slot in lane 1 than in lane 0.
  When it finds one, the period is "doomed" and is never classified.
- At every other reroll and partition site, the recorded opcode is the op
  that the gate tested.
- No record at all means that the pass left early: fewer than 4 matching
  lanes (reroll.cpp:999-1001, partition.cpp:263 and 528), a lane that is
  blocked (partition.cpp:278), or a graph without enough lane ends
  (partition.cpp:197).

---

## 1. A per-row local container (mixtures, LDA)

Variants: normal_mixture, low_dim_gauss_mix, normal_mixture_k and ldaK2:
C2__lse_local_array, C2__genk2, C2__log_mix_vector, C4__array_log_mix,
C4__vec_fresh_lse, C4__log_phi_hoisted, C4__vec_init_log_theta,
C4__matrix_then_reduce.

**What lowering produces [ran].** Each unrolled row declares its own
container, so every row gets a fresh slot. For `vector[2] lp; lp[1] = a;
lp[2] = b; target += log_sum_exp(lp);` the graph for one row is

```
s16 = NORMAL_LPDF ...          ; row value 1
s17[2] = SET_INDEX s12[2] s16 [0]      ; s12 is this row's own NaN fill
s20 = NORMAL_LPDF ...          ; row value 2
s17[2] = SET_INDEX_INPLACE s17 s20 [1]
s22 = LOG_SUM_EXP s17           ; or LOG_MIX_VEC w s17
```

The next row uses s23, s27 and s32 instead. The values that the row reduces
over live in a slot that is different in every row.

**Gates.**

| pass | file:line | what it does with this row |
|---|---|---|
| reroll | reroll.cpp:1008-1024 (`rigid_diverges`) | The functional SET_INDEX (fresh fill to row slot) and the whole-container reader (LOG_SUM_EXP or LOG_MIX_VEC) are not in the list of ops that may read per-lane slots. They read s12 and s23, so the period is doomed: `a plain op reads different slots in the first lanes`. |
| reroll | reroll.cpp:1716 | Even past that gate, LOG_SUM_EXP and LOG_MIX_VEC have no lane rule. |
| reroll | reroll.cpp:252-607 (`fuse_log_sum_exp_rows`) | This special case accepts exactly the container shape above, but only for the element grammar `log(a[i]) + log(b[j])` followed by LOG_SUM_EXP. ldaK2's base matches it. C4__log_phi_hoisted (`log_phi[k, w[n]]`, no LOGV) and C4__vec_init_log_theta (container initialised from `log(theta[doc[n]])`, then `+=`) do not. |
| partition | partition.cpp:253-256 | The reach-back stops at an op whose output slot has two writers. The row container has two (SET_INDEX and SET_INDEX_INPLACE), so the row is cut into a store lane `[INDEX, NORMAL, SET_INDEX_INPLACE]` and a one-op lane `[LOG_SUM_EXP]`. |
| partition | partition.cpp:762-776 | Both pieces read the row container, a different slot per lane that is not a scalar constant: `lanes read different non-constant slots`. Splitting by slot (line 980) gives groups of one lane. |
| partition | partition.cpp:976-977 | C2__log_mix_vector builds the row with CONCAT2, and C4__matrix_then_reduce reads a row of a matrix with SLICE_STRIDED and reduces it with LOG_SUM_EXP. Neither CONCAT2 nor LOG_SUM_EXP has a lane rule in partition: `an op has no lane rule`. Only SUM_VEC has a row-reduction arm (line 925), although `OP_LOG_SUM_EXP_ROWS` exists. |

**Condition.** Both passes require that a value which crosses from one op of
a lane to the next is a slot that the lane computes or a slot shared by all
lanes, and a row container is neither: it is a per-lane slot that is built
by stores and read as a whole.

**Reproducers [ran].** `r1_row_container.stan` with `mix12.json` (12 rows):
137 ops; reroll `a plain op reads different slots...` (OP_LOGV); partition
`lanes read different non-constant slots` (OP_SET_INDEX_INPLACE and
OP_LOG_SUM_EXP), the same as C4__vec_fresh_lse. `r1b_matrix_rows.stan`:
42 ops, partition `an op has no lane rule` (OP_LOG_SUM_EXP), the same as
C4__matrix_then_reduce.

**What the passes would need.** Treat a per-row container as one lane value
of width K:

1. Accept a chain that starts from a fill or from a whole-vector value
   (`log(theta)`), writes every element once at constant indices 0..K-1,
   and is read only inside the row. Then the row's container is a
   lane-local value of width K. The whole row becomes one lane, so the
   reach-back at partition.cpp:253 must not stop at the container's second
   writer.
2. Emit the K stores of the L rows as one [L, K] value: K strided slice
   stores, or one CONCAT of K widened columns.
3. Add a row rule for the reduction: LOG_SUM_EXP to `OP_LOG_SUM_EXP_ROWS`,
   and `log_mix(w, lp)` to `log_sum_exp(log(w) + lp)` row by row (Stan
   computes it that way). Partition needs the same rule for CONCAT2 and for
   a strided row read, which reroll already has.

This is a generalization of `fuse_log_sum_exp_rows` from one exact element
grammar to any lane-local element expression.

*Unsafe if:* the chain does not write every element before the reduction
reads it (then the NaN fill or the old values matter), the container
escapes the row, or another row reads it. *Expensive if:* K is large and
the per-element work is small, because the [L, K] layout needs strided
stores and a column-major repack. The existing LDA special case already
proves these conditions for its one grammar, at reroll.cpp:458-505.

**Note on K >= 3 [ran].** C2__genk3, C2__genk4 and normal_mixture_k's
vec_fresh variants have the same decline, but their first program is over
the 8 MiB program budget (lower.cpp:1341), which is checked after the
passes. (genk3 has 20,007 ops after lowering, about 9 MB at 448 bytes per
op.) The budget lowers the loop again as REGION_MAP, which gives 9 ops.
C2__genk2 ends with 14,207 ops (6.4 MB), stays under the budget and keeps
all its scalar ops. With `STANLI_PROGRAM_BUDGET=1000000`, genk2 also
becomes a REGION_MAP (9 ops) and runs in 60 µs instead of 112 µs (base:
45 µs). This fallback is discussed in its own section below.

**A corpus model already has this shape [ran].** The normal_mixture_k base
(K = 5, N = 1,701) declares `array[K] real ps` *outside* the row loop and
keeps 7,159 ops. One container slot is reused by every row, so the
declines differ: reroll `a lane value is read outside its lane`
(OP_LOG_SUM_EXP), and partition `the graph already used its split budget`,
because every lane writes the shared container. `fuse_log_sum_exp_rows`
accepts a reused container (reroll.cpp:458-466), but only for LDA's element
grammar. The generalization above would need to accept that form too. The
variants that move the declaration into the loop (C4__vec_fresh_K2 to K8)
get REGION_MAP from the budget and run 1.0x to 2x faster than this base.

---

## 2. One active scalar read at two positions per row

Variants: normal_mixture/C1__shared_sigma and C1__shared_sigma_manual,
low_dim_gauss_mix/C1__shared_sigma.

**Gate.** reroll.cpp:1732-1751 and partition.cpp:987-1004, both skipped
when `fuse_shared_params` (fast math) is on.

**Condition.** A scalar that carries an adjoint (a parameter, or a value
that an op writes) may be read at only one non-hoisted position of a lane.
In `log_mix(theta, normal_lpdf(y[n] | mu[1], sigma), normal_lpdf(y[n] |
mu[2], sigma))`, sigma is read by both NORMAL_LPDF positions.

**What it protects against [read].** A widened op runs one reverse step
over all L lanes before it moves to the previous position. sigma's adjoint
then receives all of position 2's contributions and then all of position
1's, where the scalar program interleaves them row by row. The comment at
partition.cpp:984-986 says that this regrouping "can lose many ULP".

**Verified by flipping the gate [ran].** `bench_grad --prep --fast-math`
turns on `fuse_shared_params`: C1__shared_sigma goes from 5,207 to 13 ops
and C1__shared_sigma_manual from 15,207 to 22. C1__shared_sigma runs in
41 µs with fast math, against 76 µs by default (base: 43 µs).

`C2__lse_pair` and `C2__naive_exp` report the same reroll decline but are
fine. They read theta twice (`log(theta)`, `log1m(theta)`), but those ops
read only invariant inputs, so partition hoists them. A hoisted position
only checks for a repeat inside its own op (partition.cpp:998), so
partition fuses those rows [ran: 15 and 17 ops].

**rats_model/C1__std_residual is not this gate [ran].** It goes from 14 to
19 ops. Partition fuses it (1,389 ops with `STANLI_NO_PARTITION=1`); only
reroll declines. The 2.6x slowdown (747 ns to 1,990 ns) comes from the
fused form itself: `normal_lpdf((y - mu) / sigma | 0, 1) - log(sigma)`
becomes SUB, DIV, an elementwise NORMAL_LPDF with 150 outputs, SUB and
SUM_VEC, where the base is one summed NORMAL_LPDF. The term is the SUB, not
the density, so partition cannot use the summed density form.

**Reproducer [ran].** `r2_shared_scalar.stan` with `mix12.json`: 67 ops, and
reroll and partition both report `an active scalar is read at two
positions` (OP_NORMAL_LPDF).

**What the passes would need.** Either of these:

- Fuse the two positions into one op over 2L elements that are laid out
  lane by lane (row 1 value 1, row 1 value 2, row 2 value 1, ...), so one
  kernel accumulates sigma's adjoint in the scalar program's order. This
  works when both positions use the same opcode (C1__shared_sigma). It does
  not work for C1__shared_sigma_manual, where sigma feeds DIV at two
  positions with other ops between them.
- Accept the regrouping and accumulate the shared adjoint with compensated
  or pairwise summation. That is at least as accurate as CmdStan's
  sequential sum when compared with a high-precision reference, but it does
  not reproduce CmdStan's rounding, and it costs a few extra flops for each
  lane.

---

## 3. Small groups walked with segment or nested loops

Variants: radon_variable_intercept_centered/C7__seg_rows2, C7__seg_rows5,
C7__nest_rows5, C7__seg_ragged1to5, and
radon_hierarchical_intercept_centered/C7__seg_rows3.

**What lowering produces [ran].** stanc3's loop vectorizer turns the nested
loop into the segment form, so both spellings give the same graph. Each
group j is one lane:

```
s19[1] = INDEX alpha [j]            ; per-group scalar
s22[5] = MUL s21[5] beta            ; s21: this group's x values, a 5-wide constant fill
s23[5] = ADD s19 s22                ; scalar + row
s24    = NORMAL_LPDF s18[5] s23 sigma
```

**Gates.**

| pass | file:line | what fails |
|---|---|---|
| reroll | reroll.cpp:1066-1078 (`row_operands_ok`), raised at 1690-1692 | ADD reads a lane-local width-1 value (`alpha[j]`, a gather) next to a width-5 row. A lane-local operand must have the row's width, unless it was hoisted: `lane widths do not agree` (OP_ADD). |
| partition | partition.cpp:762-776, using `scalar_constants` (pass_util.hpp:160) | MUL reads a different constant fill per lane (s21, s27, ...). Partition counts only fills of length 1 as constants, so a 5-wide per-lane fill is `lanes read different non-constant slots` (OP_MUL). Reroll already accepts vector constant fills (reroll.cpp:1141-1150). |
| partition | partition.cpp:740-742 | The next gate after that one, by reading: the same scalar-plus-row mix, `lane widths do not agree`. |

**Condition.** Inside a lane, every operand that varies from lane to lane
must have the same per-lane width, and a per-lane scalar is never repeated
across its lane's row.

With ragged groups (C7__seg_ragged1to5), lanes of different widths also get
different signatures and fingerprints (slot lengths are part of the key),
so only runs of equal width can match.

**Reproducer [ran].** `r3_segments.stan` with `seg12.json` (6 groups of 2):
28 ops; reroll `lane widths do not agree` (OP_ADD), partition `lanes read
different non-constant slots` (OP_MUL), the same as the variants. With
ragged lengths (1, 3, 2, 5, 4, 1, 2, 3) no record is printed, because no
width has 4 lanes.

**What the passes would need.**

1. Partition: accept per-lane constant vectors of the lane's width as
   constants, as reroll does.
2. Both: broadcast a lane-local scalar across its lane's row. The INDEX
   that produces `alpha[j]` can become a gather that repeats each index W
   times, so no new op is needed.
3. Ragged groups: partition already buckets lanes that are not adjacent, so
   with 1 and 2 each width forms its own bucket and fuses. One fused op
   over all widths would need a "repeat by group length" gather and a
   proof that the segments tile the base.

*Cost and safety:* the repeated gather scatters W adjoint contributions
back into `alpha[j]`. In the scalar program, ADD sums those W contributions
inside its kernel. The two orders agree if the scatter adds in element
order. The extra cost is the gather of L*W indices.

---

## 4. Transposes and dot products per row (C13)

### 4a. C13__transpose_row (21 to 52,825 ops, 74x slower)

`mu[n] = Z[n] * (abt')[county_idx[n]]';` puts a transpose of the whole
2 x J matrix inside the row loop.

**What happens [ran].** Lowering emits one TRANSPOSE of all 772 elements per
row (12,573 of them), followed by SLICE_STRIDED, DOT and NORMAL_LPDF.

- Reroll prints no record for this lane. By reading, the SLICE_STRIDED
  indices follow `county_idx`, so `ops_match` fails, fewer than 4 lanes
  match and reroll stops at reroll.cpp:999.
- Partition hoists the TRANSPOSE (all its inputs are invariant), and then
  declines at partition.cpp:900-910 with `lane widths do not agree`
  (OP_SLICE_STRIDED). The row read's input is the hoisted op's output,
  which partition classifies as lane-local and not as an invariant base.
  The indexed-read arm at partition.cpp:806-807 requires
  `ap.ins[0].kind == InKind::kInvariant`, so the 772-wide input counts as a
  wide operand that it cannot broadcast.
- [read] CSE (cse.cpp:240-245) does not merge the 12,573 identical
  TRANSPOSEs, because they are active. It only shares their forward value
  (`primal_source`), so every reverse pass still runs 12,573 transposes of
  772 adjoints. [ran] With `--fast-math` (`merge_active`), CSE merges them,
  and the program has 28,066 ops.

Timing [ran]: base 53 µs; transpose_row 4.49 ms by default and 333 µs with
`--fast-math`. So the per-row transposes cause most of the 74x (a factor of
about 13.5). The SLICE_STRIDED and DOT that remain per row cause the rest
(about 6x).

**Condition.** Partition requires the base of an indexed read to be the
same outside slot in every lane, and a value that the bucket itself hoists
does not count as one.

**Reproducer [ran].** `r4_transpose_row.stan` with `tr8.json`: 39 ops;
partition `lane widths do not agree` (OP_SLICE_STRIDED).

**What would fix it.**

1. Partition: treat the output of a hoisted (kShared) position as an
   invariant base, since it is emitted once and is lane 0's own slot
   (partition.cpp:1188-1189). The row read then becomes a gather. The next
   gates, by reading, are DOT, which partition has no rule for (reroll's
   GROUP_DOT arm, reroll.cpp:1666-1688, is the model for one), and the
   per-row `Z[n]` constant vector (section 3, item 1).
2. CSE: merge active ops whose reverse step only copies or permutes
   adjoints (TRANSPOSE, INDEX, SLICE, gathers without repeats). Each output
   element's adjoint receives the same contributions in the same reverse
   order, and the one copy back into the source adds the total once, so
   the result changes by at most one rounding of that final add. This item
   alone removes the 772-element reverse work per row.

### 4b. C13__colfn, colslice, row_dot, rowfn, vec_array (same op count, 1.6x)

**No lane gate [ran].** Partition fuses these. The final program has 20 or
21 ops, the same as base, but a different kernel route:

- base: GATHER alpha, GATHER beta, MUL, ADD, NORMAL_LPDF;
- colfn: CONCAT2 of alpha and beta (772), a GATHER that makes the matrix
  row-major (772), a GATHER of 25,146 elements (two per row),
  GROUP_DOT with groups of 2, NORMAL_LPDF.

Timing [ran]: base 53 µs, colfn 86 µs. The fused form gathers twice as
many elements and runs a dot product over groups of two, where base runs
two plain gathers and a MUL/ADD. A fix would recognise
`[1, x] * [a_j, b_j]'` with a constant 1 column as `a_j + x * b_j`, or
make GROUP_DOT cheap for a width of 2. Neither is a lane-decline gate.

---

## 5. Repeated rows in the data (cost model)

Scope [from `corpus-sweep/datavalues/datavalues.md`, split by reason]:
14 corpus models reach this gate. They are all 12 radon seeds (with
duplicate rows, discrete covariates, a constant or zero column, and for
most also a different index order or unbalanced groups), plus
low_dim_gauss_mix and low_dim_gauss_mix_collapse (discrete covariates
only). For example, radon_variable_intercept_centered goes from 13 ops to
23,318 with 50% duplicate rows, to 3,839 with a covariate that has 2
levels, and to 4,073 with a constant `log_radon` column.

The brief also names dugongs and grsm, but their findings show other
reasons. For dugongs (duplicate rows), the program only gains an ISLAND,
with `a store does not march...` and `lanes read different non-constant
slots`. For grsm (a constant `jj` index), the reason is `lane widths do not
agree`. Neither is this gate.

**Gate.** partition.cpp:1025-1095, which has the same formula as reroll's
`profitable` in reroll_plan.hpp:125-223 (reroll.cpp:1768-1771).

```
distinct     = number of different lanes (same outside slots and immediates)
scalar cost  = distinct * k * kLaneOpCost                 (kLaneOpCost = 5)
fused cost   = ops_out * 5 + copies + (L - distinct) * lane_elems + 40
decline when scalar cost <= fused cost
```

**What it protects against [read].** The const pool gives equal data values
one slot, so two rows with equal data compute the same lane. CSE later
merges identical lanes, and `scale_repeated_terms` multiplies the one term
that is left by its count (cse.cpp:227-235 and 267). The scalar program
therefore runs `distinct` lanes, but a fused op always runs all L lanes.
When data has a few levels (2 distinct lanes out of 10,000), the scalar
form after CSE is far cheaper. This is the case that the
`(L - distinct) * lane_elems` charge prices.

**The 431-row to 432-row flip is not caused by the duplicate [ran].** Making
the 432nd row distinct (`log_radon + 1e-3`) still gives 1,118 ops, and so
does a 433rd row, duplicate or not. Any one extra row flips it. This
contradicts the brief's description.

For radon_variable_intercept_slope_noncentered, the lane is INDEX, INDEX,
MUL, ADD and NORMAL_LPDF. Here k = 5, ops_out = 5 and lane_elems = 14, and
the two gathers add 4 per lane. The rule becomes

```
fuse when 39 * distinct > 18 * L + 65,  that is, distinct / L > about 0.465
```

- [ran] In the graph that partition receives with 431 rows, I count L = 426
  and distinct = 199 (script: `ld-work/lanes5.py`). Since 7,761 > 7,733,
  the rows fuse (27 ops). The first 431 radon rows have only 46.7%
  distinct lanes, just above the threshold.
- [ran] With 432 rows, the LANES record says that the bucket has 426 lanes
  and that partition declines on cost (1,118 ops). My count of the same
  graph finds 427 lanes of this shape with 199 distinct, and for that
  count the formula predicts "fuse".
- [inferred, not observed] A decline at L = 426 needs distinct <= 198. So
  one distinct lane probably left the bucket, but I did not find which one
  or why. The robust finding is that this data sits on the threshold,
  where one row in either direction decides the outcome.

The same row data also runs 2.2x slower as the scalar program [ran]: 3.6 µs
fused (431 rows) against 8.0 µs scalar (432 rows).

**The threshold is in the wrong place [ran].** `r5_repeated_rows.stan` has
the same likelihood, and `r5_make_data.py N D` makes N rows with D distinct
ones.

- With N = 40, D = 16 declines (47 ops) and D >= 21 fuses (11 ops).
  D = 18 to 20 give 16, 27 and 16 ops, because reroll fuses part of the run
  first, so a small N does not follow the formula cleanly.
- With N = 4,000, the formula puts the threshold at about 1,846 distinct
  rows, between the measured 1,800 (declines) and 1,900 (fuses):

| distinct rows | form | ops | ns per gradient |
|---:|---|---:|---:|
| 4,000 | fused | 11 | 17,741 |
| 1,900 | fused | 11 | 16,169 |
| 1,800 | scalar | 3,972 | 77,575 |
| 1,200 | scalar | 2,652 | 52,720 |
| 800 | scalar | 1,772 | 35,767 |
| 400 | scalar | 892 | 17,879 |
| 100 | scalar | 232 | 4,044 |

The fused form costs about 4.2 ns per lane whatever the number of distinct
lanes. The scalar form costs about 43 ns per distinct lane. Here the
measured break-even is near 10% distinct lanes; for the 431-row radon data
it is near 20% (2.2x at 46.7%). The cost model puts it at 46% in both.
Just below the model's threshold, the scalar form that it chooses is 4.8x
slower than the fused form (N = 4,000). In the cost model's units, a scalar
lane costs 39 and a fused lane 18, a ratio of 2.2. The measured ratio is
4 to 10.

**What would fix it, and whether it keeps the protection.**

- *Treat duplicate lanes as distinct* (drop the discount). This removes the
  cliff at 46%, but it loses the protection. With 2 distinct lanes out of
  10,000, the fused form would cost about 42 µs, and the scalar form after
  CSE about 0.1 µs (estimated from the per-lane costs above, not measured).
  Not safe on its own.
- *Re-price the scalar op.* Raising the scalar cost of each op about 4x
  relative to an element (or charging per-element work on the scalar side)
  moves the break-even to 10% to 20%, where the measurements put it. This
  keeps the protection, because it is still a comparison with `distinct`.
  It changes reroll's choice too, since the two share the formula, so it
  needs the corpus benchmark.
- *Share one computed lane per distinct lane* (the best form). Fuse only
  the distinct lanes: gather the D distinct rows' operands, compute one
  D-wide fused op, and replace the L terms by `dot(count, lp)`. The counts
  are integers, so this is the same multiply-by-count that
  `scale_repeated_terms` already applies after CSE, and its adjoints are
  exact. It costs about D * 4.2 ns plus one gather of D indices, which is
  lower than both current forms for any D. It needs an elementwise density
  (variant bit 6, which partition already emits for kEltDensity) followed
  by a DOT with a constant vector. For lanes that end in a store instead of
  a term, the fused D values must be gathered back to L positions, which
  costs L copies but no recomputation.

---

## 6. Row loops that read back what they write, and row-by-row matrix fills

### 6a. i319_gauss_re/C20__rowmajor_all3 (21 to 1,905 ops, 7.4x)

`mu[n] += Intercept; mu[n] += r_1_1[J_1[n]] * Z_1_1[n]; mu[n] += Xc[n] * b;`
in one row loop, with `mu = rep_vector(0, N)`.

**What lowering produces [ran].** Store forwarding merges the three stores
of a row into one. Each row is then 8 ops: `INDEX mu[n]`, ADD, INDEX r,
MUL, ADD, DOT, ADD and `SET_INDEX_INPLACE mu[n]`. The first read of `mu[n]`
stays. It reads the vector that every row writes, although its value is
still the fill's 0, because no earlier row wrote element n.

**Gates.**

| pass | file:line | what fails |
|---|---|---|
| store forwarding | inplace.cpp:288-301 | A read is forwarded only when it reads the element that the last store wrote (`hit.elem == op.idata[0]`). A read of an element that the chain never wrote is not traced back to the fill. |
| reroll | reroll.cpp:1158, then 1795-1808 | Row 0 differs from the others (a functional SET_INDEX, and a forwarded ADD), so the scan anchors at op 3, in the middle of row 0. Each lane then reads the previous lane's last value: `lanes read different non-constant slots` (OP_ADD). The retry starts one lane later, at the same phase, fails again, and the whole run is skipped. The phase that starts at a row (op 9) is never tried. [ran: records at op 3 and op 11] |
| partition | partition.cpp:590-602 and 754-757, then 543-547 and the cap at line 67 | Each lane reads `mu`, and every lane's store writes `mu`. So `mu` is "written between the lanes", and partition splits the bucket at the first writer. Each split removes about one lane, and after 32 splits: `the graph already used its split budget` (203 lanes left). |

**Condition.** Partition requires that no slot that a lane reads from
outside is written while the bucket runs, even when each lane reads only
the element that its own store writes, before it writes it.

Reroll already accepts that case: its store arm has `own_element_read`
(reroll.cpp:1308-1315). Partition has no such arm.

**Reproducer [ran].** `r6a_rmw_rows.stan` with `rmw40.json`: 324 ops, the
same records as the variant (reroll OP_ADD at period 8, partition split
budget on OP_INDEX). With a 2-column X instead of 4, island carving
compiles the rows into one ISLAND (6 ops), which hides the decline.

**What would fix it.**

1. Store forwarding: forward a read of element e through a chain of
   single-element stores that never wrote e, back to the chain's first
   base (here a constant fill). It gives the same value, it is cheap, and
   it removes the read, so reroll's phase problem goes away too.
2. Partition: accept a read of the bucket's own store vector when each
   lane reads only its own element before its own store, as reroll does.

### 6b. Mth_model/C6__row_scalar_fill_T8 (36 to 1,187 ops, no decline reported)

`for (i in 1:M) for (j in 1:T) logit_p[i, j] = mean_lp[j] + eps[i];` fills
a column-major matrix row by row. C6__row_scalar_fill (T = 5, equivalent)
has the same 1,187 ops; T = 3 is fine.

**What happens [ran].** Reroll rolls each row's inner loop on its own: T
lanes of `INDEX mean_lp[j], INDEX eps[i], ADD, SET_INDEX` become `INDEX
eps[i], ADD (T wide), SET_SLICE_STRIDED_INPLACE` (stride M). That leaves
387 three-op rows that nothing fuses again.

**Why T = 3 is fine [ran].** With T = 3 no lane pass fuses the fill
either: with `STANLI_NO_ISLAND=1`, the T = 3 variant has 4,665 ops and the
reproducer 148. Island carving compiles that scalar fill into one ISLAND
(23 and 6 ops). With T >= 4, reroll's row stores are not in the island
vocabulary, so nothing takes them.

**Gates.**

| pass | file:line | what happens |
|---|---|---|
| reroll [read] | reroll.cpp:939-948 and 1814-1825 | Periods are tried shortest first, and the first plan that fuses wins at that position. Each row's T stores march by M, so the run of period 4 is T lanes long, and T >= 4 = kMinLanes lets it fuse. With T = 3 the inner run is too short and stays scalar. |
| reroll [ran in the reproducer] | reroll.cpp:1318-1324 | The longer period (one lane per row) is refused with `a store does not march through one private vector`, for T = 3 and for T = 4. By reading, a lane may write the stored vector at only one position, and a row writes it at T positions. |
| reroll [read] | reroll.cpp:1837 | Reroll runs once, so its own output is never scanned again. |
| partition [read] | pass_util.hpp:48-51, partition.cpp:194-197 | Only single-element stores end a lane. The row stores that reroll left (`SET_SLICE_STRIDED_INPLACE`) do not, so partition finds fewer than 4 lane ends and returns without a record. |
| island [read] | island.cpp:192-216 (`in_vocab`) | SET_INDEX, SET_INDEX_INPLACE, SET_SLICE and SET_SLICE_INPLACE are accepted, but the strided slice stores are not, so the T >= 4 residue cannot be carved. |

**Condition.** Reroll requires a lane to write a stored vector at only one
position, and it keeps the first period that fuses. Partition needs
single-element stores as lane ends. The island fallback, which saves
T = 3, does not accept strided row stores.

**Reproducer [ran].** `r6b_row_fill.stan`: with `rowfill_T4.json` (12 rows,
T = 4) the program keeps 11 SET_SLICE_STRIDED_INPLACE ops (53 ops); with
`rowfill_T3.json` it has 148 scalar ops, which island carving turns into
6 ops.

**What would fix it.** Any one of:

- Add the strided slice stores to the island vocabulary. This is the
  smallest change. It gives T >= 4 the island that T = 3 gets now. That
  would remove the cliff, but the fill would still be scalar work inside
  the island.
- In reroll's store arm, let a lane write the vector at several positions
  when each position marches by its own constant stride. The whole fill
  then becomes T contiguous column stores, as in the base.
- When a short period's region ends because its stores stop marching, also
  price the longer period at the same position and keep the one that
  covers more ops. That costs more classification time during preparation.
- Let partition treat a row store with a constant width as a lane end, with
  a row-marching rule like its element-store rule (partition.cpp:847-889).

---

## 7. ch12_m12_4/C14__categorical_loop (18 to 11,934 ops, 275x)

`for (i in 1:9930) R[i] ~ categorical(p);`, with one p for every row.

**What happens [ran].** One CATEGORICAL per row (9,930 of them) and 1,987
ADD_N. In the base, `ordered_logistic` keeps its outcome in the op's
immediates. R has only 7 values, so CSE merges the rows into 7 ops times
their counts (7 ORDERED_LOGISTIC_LPMF, 7 MUL). CATEGORICAL reads its
outcome from a slot, and every row gets its own slot even when values are
equal (s55 = 4 and s59 = 4 are different slots).

**Gates.**

| pass | file:line | what fails |
|---|---|---|
| partition | partition.cpp:900-910 | p (7 elements) is the same slot in every lane. A shared operand wider than 1 must match the lane's width, and the outcome lanes have width 1: `lane widths do not agree` (OP_CATEGORICAL) [ran]. |
| partition | partition.cpp:933-949 | Past that gate, the CATEGORICAL arm needs a different row per lane (`ap.ins[1].kind == InKind::kLaneLocal`, not shared) [read]. There is no form for one shared p with an array of outcomes. |
| CSE | optable.hpp:975-990, cse.cpp:59-63 | OP_CATEGORICAL is in `is_effectful_op`, so CSE never merges equal rows [read]. |
| reroll | (no record) | Reroll prints nothing for this run [ran]. |

**Condition.** Partition fuses a categorical only when each lane has its
own probability row, and it cannot broadcast one shared row to every lane.

**Reproducer [ran].** `r7_categorical_shared.stan` with `cat12.json`: 12
CATEGORICAL ops, partition `lane widths do not agree` (OP_CATEGORICAL).

**What would fix it.** A partition arm for scalar constant outcomes with
one invariant p. It would emit one categorical over the outcome array,
which is Stan's own `categorical_lpmf(array[] int | vector)`. The simplex
check on p would then run once instead of once per row. It checks the same
value, so the program rejects the same inputs. The adjoint of p[k] would
be accumulated inside one kernel, not over 9,930 ops, which changes the
order of the additions. An equivalent form is `sum_k n_k * log(p[k])`
using the counts, which is more accurate. A narrower fix would let CSE
merge CATEGORICAL ops that have equal inputs. That needs the "boundary"
reason in optable.hpp:975-978 to be narrowed.

---

## Fallback: give the loop to REGION_MAP when the passes leave it scalar

The program budget at lower.cpp:1341 (8 MiB, applied after the passes)
already lowers a loop again as REGION_MAP when the unrolled program is too
large. That is why C2__genk3 (20,007 ops after lowering) is fine and
C2__genk2 (14,207 ops after the passes) is not. With
`STANLI_PROGRAM_BUDGET=100000` [ran]:

| variant | default ops | ops with a 100 kB budget | why not |
|---|---:|---:|---|
| 13 of the 14 variants of sections 1 and 2 (low_dim_gauss_mix/C4__vec_fresh_lse was not run) | 2,218-15,854 | 5-21 | |
| radon C7 seg and nest variants (5) | 10,572-34,375 | unchanged | REGION_MAP: `integer j is not known at compile time` (segment bounds come from data) |
| C13__transpose_row | 52,825 | unchanged | the loop is not charged to the budget (no REGION_MAP attempt is printed) |
| i319 C20__rowmajor_all3, Mth T8 | 1,905, 1,187 | unchanged | the same; by reading, the loops are not `lane_shape` loops, or are below `kMinCompactBytes` |
| ch12 C14__categorical_loop | 11,934 | unchanged | REGION_MAP: `program has effects` (CATEGORICAL) |

Timing [ran], default and then with the fallback (base in brackets):

| variant | default | fallback | base |
|---|---:|---:|---:|
| normal_mixture/C4__vec_fresh_lse | 99 µs | 59 µs (1.70x faster) | 43 µs |
| normal_mixture/C1__shared_sigma_manual | 111 µs | 55 µs (2.03x faster) | 43 µs |
| low_dim_gauss_mix/C2__genk2 | 112 µs | 60 µs (1.85x faster) | 45 µs |
| normal_mixture/C4__array_log_mix | 453 µs | 449 µs (same) | 43 µs |
| low_dim_gauss_mix/C2__log_mix_vector | 461 µs | 454 µs (same) | 44 µs |
| normal_mixture/C4__matrix_then_reduce | 55 µs | 70 µs (1.26x slower) | 43 µs |
| ldaK2/C4__vec_init_log_theta | 86 µs | 133 µs (1.54x slower) | 47 µs |

The largest relative difference in lp and gradient between the two
programs was 1.8e-14 (ldaK2) and below 2e-15 elsewhere.

So the fallback is not a fix for the group:

- It reaches only sections 1 and 2.
- It is slower than the unrolled program on two of seven.
- It costs a second lowering.
- It needs a loop charged above `min(kMinCompactBytes, budget)` and a
  `lane_shape` loop (lower.cpp:1359 and 1394-1399).
- It changes how the target is summed, because budget region maps are
  `compensated` (lower_region_map.cpp:458).

A per-loop rule ("a loop whose ops the passes left scalar, above some
count, is lowered again") would also need the corpus benchmark, since the
current budget was set from measurements of programs above 5 MB
(lower.cpp:1330-1340).

**A separate kernel cost [ran and read].** Both LOG_MIX_VEC variants take
about 450 µs whatever the program shape, against 43 µs for base. That is
about 450 ns per `log_mix(vector, vector)` call with two elements.
LOG_MIX_VEC is a generic library wrapper around Stan Math's `log_mix`
(library_fns.cpp:235-245, registered with `register_library`). With these
immediates (`[-1, 2]`) it takes the direct branch and copies nothing. I did
not read `library_fwd` or `library_bwd`, so I do not know where the time
goes. This cost is separate from the lane gates. A fused
C4__array_log_mix would still pay it, unless the row rule of section 1
rewrites `log_mix(w, lp)` as `log_sum_exp(log(w) + lp)`.

---

## Summary: gates, variants and what each needs

The evidence column says whether a tool printed this gate's decline, or
showed its effect, for the variant or its reproducer ("ran"), or whether I
found it by reading the source ("read").

| gate (file:line) | evidence | condition the variant breaks | variants | generalization |
|---|---|---|---|---|
| reroll.cpp:1008-1024 (`rigid_diverges`); partition.cpp:253-256 and 762-776; partition.cpp:976-977 (no row rule for LOG_SUM_EXP, CONCAT2); `fuse_log_sum_exp_rows` reroll.cpp:339-436 (one exact grammar) | ran (the declines and opcodes); read (which op is rigid, and the cut at 253) | a value passed between ops of a lane is a per-row container, a different slot per lane that stores build and a reduction reads | 11 cliffs of section 1, and the normal_mixture_k base itself | a per-row container of width K as a lane-local value, plus row rules for LOG_SUM_EXP (`OP_LOG_SUM_EXP_ROWS`), LOG_MIX_VEC and CONCAT2 in partition |
| reroll.cpp:1732-1751; partition.cpp:987-1004 | ran (flipping the gate with fast math gives 13 ops) | one active scalar is read at two positions of a lane | 3 (section 2) | one lane-interleaved fused op per opcode, or compensated accumulation of the shared adjoint |
| reroll.cpp:1066-1078 (raised at 1690-1692); partition.cpp:762-776 (`scalar_constants` only); then 740-742 | ran (reroll and the first partition gate); read (740-742) | a lane mixes a per-lane scalar with a width-W row, and partition knows only scalar constants | 5 (section 3) | vector constant fills in partition; broadcast a lane scalar with a repeated-index gather |
| partition.cpp:806-807 and 900-910; cse.cpp:240-245 | ran (the decline, and a 13.5x speedup when fast math merges the transposes); read (806-807) | an indexed read's base is a hoisted value, not an outside slot; CSE does not merge active transposes | C13__transpose_row | hoisted outputs count as invariant bases; CSE merges ops whose reverse step only copies or permutes adjoints |
| none (kernel route) | ran | | C13 colfn, colslice, row_dot, rowfn, vec_array; rats C1__std_residual | kernel level: `[1, x] . [a, b]` as `a + x * b`; a summed density form for `lpdf - c` |
| partition.cpp:1025-1095, the same formula as reroll_plan.hpp:194-222 | ran (formula checked at N = 4,000; break-even measured) | distinct lanes / lanes > about 0.465 for a 5-op lane | 14 corpus models with repeated data values: the 12 radon seeds, low_dim_gauss_mix and its _collapse | re-price the scalar op (measured break-even 10% to 20%), or fuse only the distinct lanes with integer counts |
| inplace.cpp:288-301; reroll.cpp:1158 and 1795-1808; partition.cpp:590-602 and 543-547 | ran (the declines and their positions); read (the forwarding rule) | each lane reads the element that its own store writes, from the vector that every lane writes | i319 C20__rowmajor_all3 | forward reads of never-written elements to the fill; partition's own-element read, like reroll.cpp:1308-1315 |
| reroll.cpp:939-948 and 1825; reroll.cpp:1318-1324; pass_util.hpp:48-51; island.cpp:192-216 | ran (the row stores and the 1318 decline in the reproducer, T = 3 without islands); read (period order, vocabulary) | the shortest period wins; a lane may write a stored vector at only one position; row stores end no partition lane and cannot be carved into an island | Mth C6__row_scalar_fill (T 5, T 8) | strided stores in the island vocabulary (smallest), or multi-position stores in reroll's store arm |
| partition.cpp:900-910, then 933-949; optable.hpp:975-990 | ran (900-910); read (933-949, CSE) | one shared probability vector for scalar-outcome lanes | ch12 C14__categorical_loop | a categorical arm for a shared theta with an array of outcomes |

**Which single generalization recovers the most.**

- *By number of cliffs:* treat a per-row container as a lane-local value of
  width K, with row rules for the reductions (section 1). It covers 11 of
  the 28 cliffs in this group. It also covers the K >= 3 variants that the program
  budget saves today, and it replaces the special case for one LDA grammar.
  It is also the largest change: lane segmentation, a width model and new
  emit forms.
- *By number of real models, for the least code:* re-price the cost model
  (section 5). It is one formula, which both passes share. It affects 14
  corpus models whose data has repeated rows, discrete covariates or a
  constant column. Measured, its break-even is 10% to 20% distinct lanes,
  where the model has 46%.
- *Shared by several sections:* partition's width model (sections 3, 4a and
  7, and part of 1). Partition's width gates are at partition.cpp:740-742,
  782-785 and 900-910. With vector constants, a lane scalar broadcast across
  its row, and a shared wide operand, partition would get past the first
  gate in 7 more cliffs. Each of those still needs one more rule after it
  (DOT, CATEGORICAL with a shared theta).

## Files

- `lane-declines/r1_row_container.stan`, `r1b_matrix_rows.stan`,
  `r2_shared_scalar.stan` with `mix12.json`
- `lane-declines/r3_segments.stan` with `seg12.json` and `seg_ragged.json`
- `lane-declines/r4_transpose_row.stan` with `tr8.json`
- `lane-declines/r5_repeated_rows.stan` and `r5_make_data.py N D OUT.json`
- `lane-declines/r6a_rmw_rows.stan` with `rmw40.json`
- `lane-declines/r6b_row_fill.stan` with `rowfill_T3.json` and `rowfill_T4.json`
- `lane-declines/r7_categorical_shared.stan` with `cat12.json`
- Working files (stage dumps, sweeps): `rootcause/ld-work/`
