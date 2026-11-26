# Why the row loop falls back to OP_LOOP or to one ISLAND per row

Worktree `the perf/perturbation-fuzz worktree` at b046d6169, tools from
`build-rel`. Reproducers and data are in `loop-fallback/` next to this file.
`loop-fallback/dm.sh MODEL.stan DATA.json [summary]` compiles with the same
embedded probe that the harness uses (`--vectorize-loops on`) and runs
`dump_ops` with `STANLI_LANE_REPORT=1 STANLI_REGION_MAP_DIAGNOSTICS=1`.
`loop-fallback/bg.sh MODEL.stan DATA.json` runs `bench_grad --timed` for one
second at two points and prints ns per gradient. Data paths must be absolute,
because `bg.sh` changes directory first. Timings are single short runs with a
machine load of about 7 to 8, so read them as plus or minus 10%.

"Verified" below means that I ran something and saw the result. "Inferred"
means that I read it in the source and did not run a test that isolates it.

## Summary

| # | Gate (file:line) | Variants it explains | What the variant breaks |
| --- | --- | --- | --- |
| A | `lower_region_map.cpp:369-375` (assigns outer name), `:367-368` (continue) | cogmod C3 (all 14); cogmod C12 `_transforms_inside`; last step of radon C21 | The map needs every name that the body assigns to be declared in the body, and no `continue`. |
| B | `adjoint.cpp:62-64` (back edge), then `lower_region_map.cpp:390` and `lower_structured_loop.cpp:2984` | cogmod C12 (plain, depth 2 to 4) | The generated backward needs straight-line code; an inner loop with a bound read from data by the outer index leaves a back edge. |
| C | `inplace.cpp:288-304` (forwarding stops once the vector is read) | radon C21 `other_elem_dead` (3 seeds) | Forwarding needs no read of the vector between the store and the read-back; `mu[1]` is such a read. |
| D | `mir_prog_expr.cpp:314-322` (row copy) feeding `lower_region_map.cpp:472` (16,384 cap); prep: `lower_stmt.cpp:2482-2484` | radon C11 `inner_d_loop_D17` | `Y[d][n]` copies all N cells of `Y[d]` into the body program, so the body passes the instruction cap. |
| E | `lower_structured_loop.cpp:2822-2833` (inner carry inherited), then `lower_stmt.cpp:2493`, `:2517-2535` | diamonds C23 `inner_loop`; also any row loop with an inner sum over enough rows | The large-loop rule retains a loop that "carries a value", and an inner loop's carry counts even when it never leaves the row. |
| F | `reroll.cpp:1272-1281` (refused at `:1329`), `partition.cpp:847-858` (refused at `:877`) | kidscore_momhsiq C8 reversed/sorted, s2_index_mi C8 reversed | A run of stores into one vector needs indices that step by a constant positive stride. |
| G | `lower_stmt.cpp:1294-1297` (fold whitelist), `lower_region_map.cpp:366`, `lower_structured_loop.cpp:3319-3324`, `cse.cpp:59-62` | C16 `check_loop_1e4` (4 seeds) | Every path refuses a `reject`, even one that is the same in every iteration. |
| H | `lower_stmt.cpp:2403` with `lower_region_map.cpp:395` (selection, not refusal); `region_map_lanes.cpp:34-70` (no `NE`) | C15 `udf_early_return1/2` | The map takes any loop whose branch reads a parameter, even a branch that is the same in every row; moving that test out of the loop by hand gives 1.5 µs against 9.3 µs. |

Every corpus variant named in the task was run through `dm.sh` with both
diagnostic switches, and each printed the refusal or selection that its row
gives.

### Why the harness shows no decline reason

`measure.py:270` sets only `STANLI_LANE_REPORT=1`, which prints the
declines of reroll and partition. The refusals in A, B, D and G print only
under `STANLI_REGION_MAP_DIAGNOSTICS=1` (map) or
`STANLI_STRUCTURED_LOOP_DIAGNOSTICS=1` (OP_LOOP, unroll, retention). Three
decisions print nothing under any switch: the forwarding stop in C, the
refusal of the loop-invariant target fold in G, and the selection in H.
Setting both diagnostic switches in `structure()` would give A, B, D, E and G
a reason in `results.json`.

### "The graph already used its split budget"

This is `kMaxSplits = 32` in `partition.cpp:67`, checked at `:545` and
`:567`. When a bucket of lanes breaks at an op, partition splits the bucket
there and tries both halves again; after 32 splits per graph it stops. In C21,
C8 (s2_index_mi) and C16 it is a bystander. This is inferred from reading
`partition.cpp`; I did not raise the cap to test it. The graph that partition receives
was already broken by the gate in the table (thousands of islands in C16, a
store chain in C21, a refused store in C8), and partition has the same stride
rule for stores as reroll, so more splits would not fuse those lanes.

## A. The map refuses a body that assigns an outer name or uses `continue`

**Variants.** All cogmod `C3__*` (move1 to move5, `continue_guard_all_in_loop`,
`store_loglik_all_in_loop`, `store_loglik_vector_transforms`) in cm_exwald,
cm_weibull, cm_gamma and cm_lnr; the C12 `_transforms_inside` variants (the
map refuses them for A before it reaches B); and the final compact step in
C21. `continue_guard_all_in_loop` also moves the transforms into the loop, so
it needs both fixes below.

**Verified.** Under `STANLI_REGION_MAP_DIAGNOSTICS=1` every C3 variant prints
`stanli_region_map refused (n): assigns mu declared outside the body` (or
`sigmazero` for cm_lnr, `log_lik` for `store_loglik_vector_transforms`), and
`continue_guard_all_in_loop` prints `break, continue or return`. The loop then
reaches `try_lower_region` at `lower_stmt.cpp:2404`, which keeps it as OP_LOOP
because it branches on a parameter.

**Gate.** `region_map_shape`, `lower_region_map.cpp:367-368` (any
`continue` that is not inside an inner loop, via `body_escapes` at `:17-27`)
and `:369-375` (any assigned name that is not declared in the body and is not
the accumulator).

**Condition.** The map requires that the body assign only names that it
declares itself; `mu[n] = log1p_exp(mu[n])` writes an element of a vector that
is declared before the loop.

**Reproducer** (`g1_assign_outer.stan`, data `rows.json` or `rows2000.json`;
base `g1_base.stan` has `mu = exp(mu);` before the loop):

```stan
data { int N; vector[N] y; }
parameters { real a; real<lower=0> s; }
model {
  vector[N] mu = rep_vector(a, N);
  for (n in 1:N) {
    mu[n] = exp(mu[n]);   // the store into an outer vector refuses the map
    if (y[n] > mu[n]) target += normal_lpdf(y[n] | mu[n], s);
    else target += normal_lpdf(y[n] | mu[n], 2 * s);
  }
}
```

`g1_continue.stan` adds `if (y[n] <= -5) continue;` to the base instead.
Both lower to OP_LOOP. The cost in these small bodies is small: at N = 2000,
base 90 µs, `assign_outer` 129 µs, `continue` 95 µs. With a heavier body
(`g1_heavier_*`, a user density with three parameter branches) the gap is
2.1x (106 µs against 225 µs). The corpus variants have much larger bodies and
show 2x to 5x. At fixed benchmark points OP_LOOP does not re-record; under
sampling a branch that flips makes it record again, so these numbers are a
lower bound.

**What the map would need.**

- Assignments: treat `x[n]` as a private scalar of iteration n when every read
  and write of `x` in the body uses exactly the loop variable as the index,
  and `x` is not read after the loop. In all C3 variants `mu`, `log_lik` and
  the other transformed vectors are model-block locals that nothing reads
  after the loop. If `x` is read after the loop, the map has to write a
  vector output with its own adjoints, but today it has one scalar output
  (`out_regs`, `lower_region_map.cpp:429`). That is the expensive part.
  Inferred: once line 373 lets the write through, `:488-489`
  ("program writes a live-in register") refuses next, because `mu` is a
  live-in; privatizing the element avoids that write.
- `continue`: compile a top-level `continue` as a forward jump to the end of
  the iteration. `gen_adjoint` accepts forward jumps (it refuses only back
  edges, see B), and the lane form already masks lanes on forward branches.
  `break` and `return` must stay refused, because they change which later
  iterations run. This change is cheap and has no safety cost that I can see.

## B. An inner loop with a data-dependent bound leaves a back edge

**Variants.** cogmod `C12__subjects_balanced_depth2`, `_ragged_depth2`,
`_ragged_depth3`, `_ragged_depth4` (ops 16 to 1055..2063, 200 ISLANDs each).

**Verified.** The map prints `refused (s): no generated adjoint
instructions=640` (68 to 3037 by model) and names no opcode and no call
without a backward. `STANLI_STRUCTURED_LOOP_DIAGNOSTICS=1` then shows the
outer loop unrolled (`trips=8` or `10`, or `4` at depth 3) and each inner loop
unrolled, which leaves one island per row (200 rows, 200 islands). With a
constant inner bound (`g2_control_constant_inner_bound.stan`, `for (t in
1:10)`) the same loop maps (`body=202`).

**Gate.** `gen_adjoint`, `adjoint.cpp:62-64`: a jump to an earlier
instruction returns false ("A flag per basic block suffices only when each
block runs at most once"). This is inferred from the refusal text and the
control, because the diagnostic does not name the back edge; the other
reasons in `gen_adjoint` (opcodes without adjoints, calls without a backward)
would have been listed. Two more rules then decide the fallback:
`region_auto_profitable` (`lower_structured_loop.cpp:2984`) refuses OP_LOOP
for an outer loop of fewer than 32 trips, and the inner loops have lexical
depth 2, so neither the map (`lower_region_map.cpp:390`) nor OP_LOOP
(`lower_structured_loop.cpp:2981`) considers them. `structured_outer_depth`
counts lexical nesting (`lower_internal.hpp:2921`), so it stays 2 even after
the outer loop is unrolled.

**Condition.** The generated backward requires that each block run at most
once per iteration; `for (t in 1:nobs[s])` with `s` held in a register makes
the inner loop a runtime loop in the body program.

**Reproducer** (`g2_nested_ragged.stan`, data `groups.json`):

```stan
data { int N; vector[N] y; int G; array[G] int len; array[G] int start; }
parameters { real a; real<lower=0> s; }
model {
  vector[N] mu = exp(rep_vector(a, N));
  for (g in 1:G) {
    for (t in 1:len[g]) {          // inner bound read from data by g
      int n = start[g] + t - 1;
      if (y[n] > mu[n]) target += normal_lpdf(y[n] | mu[n], s);
      else target += normal_lpdf(y[n] | mu[n], 2 * s);
    }
  }
}
```

64 islands, 5.2 µs per gradient. The same 64 rows as one flat loop
(`g1_base.stan` on `rows.json`) take 2.8 µs. Unrolling the outer loop by hand
(`g2_control_outer_unrolled_by_hand.stan`) gives four maps and 4.5 µs.

With 40 groups (`groups40.json`, 224 rows) the outer loop has 32 or more
trips, and it becomes OP_LOOP instead of islands (verified): 10.7 µs, against
9.6 µs for the flat loop on the same rows (`rows224.json`). Real models often
have more than 32 subjects, so they get OP_LOOP. At fixed points OP_LOOP
replays one recording; under sampling it records again whenever a branch
flips, so this gap understates the cost in a real run.

**What would be needed.**

- Best: flatten a perfect nest whose inner bounds are data known at lowering
  into one map over all (s, t) pairs, with the row index per iteration taken
  from a table. Cost: one integer table per iteration (O(N) memory). It is
  safe only when the outer body has no statements besides the inner loop,
  or when those statements can be repeated per pair.
- Cheaper: let the map take a loop whose enclosing loops have all been
  unrolled, since its bounds are then fixed. This gives one map per outer
  iteration. It compiles the body once per outer iteration and pays a tile
  per small group, so it gains less (4.5 µs against 5.2 µs here).
- A generated backward for real loops would need a tape for each run of a
  block, which is the costly route.

## C. Store-to-load forwarding stops after a read of another element

**Variants.** `radon_county_intercept`, `radon_variable_intercept_centered`
and `radon_variable_intercept_slope_noncentered` `C21__other_elem_dead`
(about 20x slower, +LOOP).

**Verified.** On `radon_tiny.json` with `STANLI_PROGRAM_BUDGET=0`, the base
folds to 6 ops (one GATHER, one vector density). The variant keeps 49 ops:
in iteration 1 the density reads the stored value directly, but from
iteration 2 on it reads `INDEX mu[n]` from the in-place vector, and every
iteration keeps `INDEX mu[1]` and a `SUB` for the dead `d[n]`. Reroll then
declines `NORMAL_LPDF` ("lanes read different non-constant slots"). The
sibling variants show what the condition is. `C21__dist1/2/4` put 1 to 4 statements
between the store and the read and are fine, and so is a dead `d[n] = mu[n] -
x[1]` (`g3_control_dead_own_elem.stan`, 7 ops). A live `d` that reads `mu[1]`
(`g3_live_other_elem.stan`) breaks in the same way (61 ops).

On the real data, the unrolled loop has 103,102 ops after the passes, about
46 MB, which is over both budgets. In the compact retry the map's lane form
refuses `assigns mu declared outside the body` (gate A), and above 32 MiB the
loop becomes OP_LOOP.

**Gate.** `forward_stores_to_loads`, `inplace.cpp:288-304`. A read of `mu`
at a different element falls through to `:303-304` and sets `observed` on
the pending store, and an observed store is never forwarded (`:294`). The
dead-store sweep at `:307-322` then deletes the `d` stores but not the ops
that computed their values, and no later pass removes them.

**Condition.** Forwarding requires that nothing read the vector between the
store and its read-back; `mu[1]` is such a read, even though it reads a
different element and its only consumer is dead.

**Reproducer** (`g3_dead_other_elem.stan`, data `radon_tiny.json` for the
structure, `radon_big.json` for timing; base `g3_base.stan`):

```stan
data { int N; int J; array[N] int<lower=1, upper=J> idx; vector[N] x; vector[N] y; }
parameters { vector[J] alpha; real beta; real<lower=0> sigma; }
model {
  vector[N] mu;
  vector[N] d;
  for (n in 1:N) {
    mu[n] = alpha[idx[n]] + beta * x[n];
    d[n] = mu[n] - mu[1];          // never read; reads another row's mu
    target += normal_lpdf(y[n] | mu[n], sigma);
  }
}
```

On 12,000 rows: base 40 µs, variant 943 µs (23.5x), with LOOP.

**What would be needed.**

- Dead `d`: remove ops whose results nothing reads, repeating until nothing
  changes, before forwarding. The `mu[1]` read then disappears with the dead
  `d` store, and the graph becomes the base graph (inferred from the
  own-element control). `cse.cpp:69` already has `droppable_if_unread`. The
  sweep has to treat the in-place store chain as one value, because each
  `SET_INDEX_INPLACE` reads the slot that it writes (`in[0] == out`), so a
  plain "is the output read" test sees every store in the chain as live.
  This is cheap and safe for ops without effects.
- Live `d`: forwarding would have to track the last store per element, so
  that `mu[1]` forwards to iteration 1's value. Reroll would then see lane 1's
  value read by every lane, which today is "a lane value is read outside its
  lane", and it would have to treat that value as an invariant input. The
  comment at `inplace.cpp:290-293` explains why `observed` exists: it keeps a
  later loop's reads from giving the last lane an outside reader. A per-element
  version has to keep that property.
- Gate A would also give the compact step the lane form here, which would
  avoid OP_LOOP even without a forwarding fix.

## D. `Y[d][n]` copies the row into the body program

**Variants.** `radon_county_intercept/C11__inner_d_loop_D17` (296x slower;
CmdStan 17.5x; preparation 48 ms to 4.6 s).

**Verified.** The first lowering prints `unrolled loop=d` up to about 1.0M
ops and stops at the unroll ceiling. The compact retry prints `refused (n):
body program too large`, and the loop becomes OP_LOOP. In
`g4_inner_data_loop.stan` the map body grows with N: 193 instructions at
N = 40 and 1,633 at N = 400 (`STANLI_REGION_MAP=all`). `dump_islands` shows
40 `MOV`s that copy `Y[d]` before each `DYN_INDEX`. Preparation of the radon
variant takes 4.97 s, and 0.30 s with `STANLI_UNROLL_CEILING=50000`, so about
4.7 s goes to unrolling up to the 2^20 ceiling and then discarding the result.

**Gate.** The array selection in `mir_prog_expr.cpp:298-322` resolves
`Y[d]` (fixed `d`, vector leaf) to a contiguous map and copies all `width`
cells with one `MOV` each (`:314-322`). With N = 12,573 and D = 17 the body
needs about 214,000 instructions, and `lower_region_map.cpp:472` refuses above
`kMaxRegionMapCode = 16384` (`:196`). The 4.7 s comes from the ceiling check
at `lower_stmt.cpp:2482-2484`, which compares the ops emitted so far with
2^20 after each trip, so it fails only after it has emitted 2^20 ops.

**Condition.** The map requires a body of at most 16,384 instructions; a read
`Y[d][n]` of an array of vectors costs N instructions per `d`, so the body
grows with the data.

**Reproducer** (`g4_inner_data_loop.stan`, data `inner_small.json`,
`inner_mid.json`, `inner_big.json` with N = 12,000 and D = 17):

```stan
data { int N; int J; int D; array[N] int<lower=1, upper=J> idx; vector[N] x; array[D] vector[N] Y; }
parameters { vector[J] alpha; vector[D] beta; vector<lower=0>[D] sigma; }
model {
  for (n in 1:N) {
    for (d in 1:D) {                 // a data-sized inner loop inside the row loop
      Y[d][n] ~ normal(alpha[idx[n]] + beta[d] * x[n], sigma[d]);
    }
  }
}
```

On `inner_big.json` it takes OP_LOOP and 12.6 ms per gradient. The radon
variant takes 11.9 ms, against 40 µs for the base and 0.69 ms for
`C11__outloops_D17` (the same work with the loops swapped). Plain unrolling
is not a fix: with `STANLI_STRUCTURED_LOOPS=0` the reproducer folds to 8 ops
and 1.87 ms, but preparation takes 16.4 s and 39.8 GB of peak memory. I did
not run that setting on the radon variant.

**What would be needed.**

- Do not copy a contiguous selection. Return a view of the live-in
  registers (`b.reg + map.offset`, `width`) when the selection is contiguous,
  or fold `Y[d][n]` into one `DYN_INDEX` with a base offset, as the code at
  `mir_prog_expr.cpp:274-297` already does for arrays of reals. A view
  shares registers with a live-in, so a body that writes it would change the
  input; the check at `lower_region_map.cpp:488-489` already refuses that
  case. This change is cheap.
  A nearby spelling shows what the map does at this size. With `array[D, N]
  real Y` and `Y[d, n]` (`g4_control_flat_array.stan`, verified), stanc
  vectorizes the inner loop in another way (6 ops per row), the first
  lowering stays under the ceiling, and the budget sends the loop to the
  map's lane form (`body=43`): 4.35 ms per gradient against 12.6 ms for
  OP_LOOP, and 0.16 s of preparation. This is not the exact program that the
  fix would produce, so treat 4.35 ms as an estimate. It is still 6x slower
  than the swapped loops (0.69 ms on radon), which reroll folds into vector
  densities.
- Preparation: estimate the unrolled size from the first iteration (the
  iteration count times the ops of one iteration) and stop at once when it
  passes the ceiling.

## E. An inner loop's private carry makes the row loop "carry a value"

**Variants.** `diamonds/C23__inner_loop` (100x slower; CmdStan 36x). The rule
is broader than that variant: a scalar accumulator that the row body
declares sets it off too.

**Verified.** `STANLI_STRUCTURED_LOOP_DIAGNOSTICS=1` prints `unrolled loop=n
trips=5000 ops=605007 carries=1 dep=1` and then `large loop retained loop=n`.
With `STANLI_STRUCTURED_LOOPS=0` the same variant folds to 80 vector ops
(24 MUL and ADD pairs over 5000 rows, then one density). Timing: base 41 µs,
retained 7.5 ms, unrolled 133 µs (3.2x the base, where CmdStan is 35.6x).
Preparation: retained 0.23 s and 196 MB, unrolled 0.38 s and 289 MB.

`g4c_inner_sum_local.stan` uses `real acc` declared in the row body and also
reports `carries=1 dep=1`. At 5,000 rows it has 365k raw ops, which is below
the threshold, so it unrolls and folds (142 µs). At 8,000 rows it has 584k
raw ops, is retained, and takes 4.2 ms against 228 µs unrolled (18.5x).

**Gate.** `loop_carries_value`, `lower_structured_loop.cpp:2822-2833`: if
any inner `for` carries a value relative to itself, the outer loop is marked
as carrying (`if (inner) return true;`), with no check of where that value
lives. `log_prob_loop_may_retain` (`:2967-2971`) then lets the loop be
retained, and `log_prob_loop_retained` (`:2973-2976`: graph of at least 2^19
raw ops, the loop at least 2^14, at least 32 per trip) retains it at
`lower_stmt.cpp:2517-2535`. This choice uses the raw op count before the
passes run. The comment at `lower_internal.hpp:2863-2867` says that a carried
value "leaves a serial chain they cannot fold", which is not true here.

**Condition.** Retention assumes that a loop which carries a value cannot be
folded; the row loop counts as carrying whenever an inner loop does, even
when the inner loop only adds into `mu[n]` or into a scalar that the row body
declares.

**Reproducer** (`g4c_inner_sum_local.stan`, data `inner_sum8k.json`;
`g4b_inner_sum_vector.stan` on `inner_sum.json` is the diamonds form):

```stan
data { int N; int K; matrix[N, K] X; vector[N] y; }
parameters { real a; vector[K] b; real<lower=0> s; }
model {
  for (n in 1:N) {
    real acc = a;
    for (k in 1:K) acc += X[n, k] * b[k];     // acc is declared in the row body
    target += normal_lpdf(y[n] | acc, s);
  }
}
```

The threshold of 2^19 raw ops sets the size at which this appears: about
4,300 rows at K = 24 for the vector form (121 raw ops per row), and about 7,200 for the local form.

**What would be needed.** When an inner loop carries, check whether every
name that it carries is private to one outer iteration: declared in the outer body, or
written and read only at an index that is exactly the outer loop variable
(`own_element` at `:2840-2863` already makes this check for direct reads).
If so, the outer loop does not carry. This is cheap. The case that must
stay retained is a carry that does leave the row, such as `acc` declared
before the outer loop, or `mu[n - 1]`.

## F. A run of stores into one vector must step by a constant positive stride

**Variants.** `kidscore_momhsiq/C8__mu_loop_reversed`, `_sorted_order`;
`s2_index_mi/C8__mu_loop_reversed`, `C8__loop_store_and_reversed_mu`.

**Verified.** These do report a decline: `reroll ... OP_SET_INDEX_INPLACE
... a store does not march through one private vector` at every period, and
for kidscore the same from partition. The scalar residue becomes one island.
`dm.sh` on both s2_index_mi variants shows the same reroll decline.

**Gate.** `reroll.cpp:1272-1281`: the stride is the difference between the
first two lanes' indices, and the run requires `stride >= 1` and then each
index equal to `start + l * stride`. A failed run is refused at `:1329`.
Partition repeats the rule at `partition.cpp:847-858` and refuses at `:877`.

**Condition.** Reroll requires that the store indices increase by a constant
step; a reversed loop steps by -1, and a loop in sorted order writes a data
permutation.

**Reproducer** (`g5_reversed_store.stan`, data `rows400.json`; control
`g5_control_forward_store.stan` has `int n = i;`):

```stan
data { int N; vector[N] x; vector[N] y; }
parameters { real a; real b; real<lower=0> s; }
model {
  vector[N] mu;
  for (i in 1:N) {
    int n = N - i + 1;               // the store index runs N, N-1, ..., 1
    mu[n] = a + b * x[n];
  }
  y ~ normal(mu, s);
}
```

4.45 µs against 0.91 µs for the forward loop (4.9x).

**What would be needed.** When the store indices are a permutation of a
range that the run covers (each element written once), emit the values in
lane order and build the vector with one `OP_GATHER` through the inverse
permutation. `OP_GATHER` exists, and its backward adds into the selected
elements. A stride of -1 is the special case of a reversal. It is unsafe when
an index repeats (the last write wins) or when the run does not cover the
range (the unwritten elements keep the old values), so both cases must stay
refused. The cost is one integer table of N entries. A gather of this size is
fast in practice: `s2_index_mi/C8__mu_gather_vector`, which writes the same
update as one indexed read (`mu_y += bsp_y[1] * Yl_yposmi[idx]`), runs at
1.04x the base in `results.json`. That variant reads through an index and
does not store through a permutation, so it is evidence about the cost of a
gather, not a test of this fix.

## G. A `reject` in a loop that repeats the same body

**Variants.** `C16__check_loop_1e4` in aalto_grp_aov, aalto_lin,
eight_schools_centered and kidscore_momiq (10,000 ISLANDs, about 30,000
lowered ops).

**Correction to the premise.** The check is not data-only. In all four seeds
the condition reads a parameter: `is_nan(sigma) || sigma <= 0` (or `tau` in
eight_schools). So the loop branches on a parameter and is a map candidate.
Each iteration is identical; nothing depends on `m`.

**Verified.** With both diagnostic switches the variant prints
`stanli_region_map refused (m): body has effects`, then `stanli_structured
refused: native: structured body requires the per-draw fallback for a
stateful call`, then `unrolled loop=m trips=10000`. The loop-invariant target
fold that would collapse it into one body times 10,000 is not taken, and
prints nothing. `dump_islands` on the reproducer shows 1,000 identical
five-instruction islands (`LE`, `JZ`, `REJECT`) with the same input slot.

**Gates.**

- The loop-invariant fold, `repeatable_target_stmt`: `reject` is a statement
  call and falls into `default: return false` at `lower_stmt.cpp:1294-1297`.
  The documented whitelist (`OPTIMIZATIONS.md`, "Loop-invariant target
  collapse") excludes checks, `print` and `reject`.
- The map: `stmt_effectful` at `lower_region_map.cpp:366`.
- OP_LOOP: any `OP_REJECT` in the body at
  `lower_structured_loop.cpp:3319-3324`.
- After unrolling, each parameter `if` becomes its own island, and CSE never
  merges islands (`never_merge`, `cse.cpp:59-62`), so all 10,000 identical
  islands run.

**Condition.** The fold requires that the body hold only declarations,
assignments to locals, target increments, `if`, `for` and `skip`; one
`reject` disqualifies it even when the reject is the same in every iteration.

**Reproducer** (`g6_invariant_reject.stan`, data `rows.json`; control
`g6_control_no_reject.stan` drops the `if` line):

```stan
data { int N; vector[N] y; }
parameters { real a; real<lower=0> s; }
model {
  y ~ normal(a, s);
  for (m in 1:1000) {              // every iteration is the same
    if (s <= 0) reject("s must be positive, got ", s);
    target += cauchy_lpdf(s | 0, 2.5) / 1000;
  }
}
```

25.4 µs against 0.10 µs without the check (1,006 ops against 6).

The `reject` alone blocks the fold (verified). Keeping the `if` and replacing
the reject with `target += 0;` folds to 4 ops, both with `s <= 0` and with
the corpus condition `is_nan(s) || s <= 0`. With the reject and the corpus
condition, the loop unrolls again (1,006 ops). So `lower_stmt.cpp:1294-1297`
is the only gate in the fold for these variants. I ran all four corpus seeds
through the diagnostics, and each prints the same two refusals.

**What would be needed.** Let the fold accept a `reject` when its condition
and message do not read the loop variable or anything that the body
assigns. Running such a body once raises the same error at the same point,
which is the first iteration, and the fold keeps the reject before the
target term, as in the source. `print`, RNG calls and `target()` must stay
refused, because the number of times they run is visible. This is cheap. A
weaker fix would let CSE merge identical islands that have no adjoint.

## H. The map is chosen for a branch that is the same in every row

**Variants.** `aalto_lin/C15__udf_early_return1`, `_return2`,
`kidscore_momiq/C15__udf_early_return1`, `s2_car/C15__udf_early_return1`.
Control: `cm_weibull/C15__udf_early_return1` is fine, because its base
already uses the map.

**Verified.** Nothing is refused; the map is selected (`lanes=save` for
`return1`). The profile (`STANLI_REGION_MAP_PROFILE=1`) shows that `LE` and
`JZ` read only shared operands in 100% of tiles, so every row takes the same
arm. Three spellings on 434 rows (`rows434.json`):

| model | form | ns per gradient |
| --- | --- | ---: |
| `g7_no_guard.stan` | unrolled and rerolled, one vector density | 977 |
| `g7_control_hoisted_by_hand.stan` | `if` outside the loop, one island with a vector call | 1,497 |
| `g7_guard_on_scale.stan` (`if (s <= 0)`) | map, lane form | 9,321 |
| same, `STANLI_REGION_MAP_TILE_CALLS=0` | map, lanes, scalar calls | 19,214 |
| same, `STANLI_REGION_MAP_LANES=0` | map, scalar form | 38,292 |
| `g7_guard_on_mu.stan` (`if (is_nan(mu))`) | map, lanes refused for `NE` | 39,432 |

So the tiled density call is already in use. The remaining cost is the
per-lane interpreter and the backward, not a missing kernel. With the map off
(`STANLI_REGION_MAP=0`), kidscore C15 takes 27 µs against 9.9 µs with it,
so the map is already the best form that exists today.

`return2` adds `if (is_nan(mu))`. In the map program it becomes
`NE r873 <- r871, r871` (a value compared with itself; seen with
`dump_islands` on `g7_guard_on_mu.stan`). `NE` is missing from
`lane_opcode` (`region_map_lanes.cpp:34-70`; `EQ` is at `:58`), so the lane
plan refuses and the map runs the scalar form. That is why `return2` is
slower than `return1` on aalto_lin (7.5x against 5.6x).

In absolute time stanli stays faster than CmdStan on kidscore_momiq (0.65 of
CmdStan's time) and s2_car (0.69). It is slower only on aalto_lin, where
N = 8 (1.44 and 1.98 times CmdStan's time).

**Gate.** `lower_stmt.cpp:2403` tries the map before the loop is unrolled,
and `lower_region_map.cpp:395` accepts it when `ParameterControl`
(`:50-194`) finds a branch that reads a parameter. `ParameterControl` never
checks whether the condition varies with the loop variable. Here `s` is bound
to `sigma` at the call site, so the condition is the same in every row.

**Condition.** The selection requires only that a branch read a parameter;
the base needs the loop to be unrolled so that reroll can build one vector
density, and a guard on a scalar parameter sends it to the map first.

**Reproducer** (`g7_guard_on_scale.stan`, data `rows434.json`):

```stan
functions {
  real guarded_lpdf(real y, real mu, real s) {
    if (s <= 0) return negative_infinity();   // the same test in every row
    return normal_lpdf(y | mu, s);
  }
}
data { int N; vector[N] x; vector[N] y; }
parameters { real a; real b; real<lower=0> s; }
model {
  vector[N] mu = a + b * x;
  for (n in 1:N) target += guarded_lpdf(y[n] | mu[n], s);
}
```

**What would be needed.**

- `return2`: add `NE` to the lane opcodes, as `EQ` is. This is cheap.
- `return1`: move a condition that is the same in every iteration out of the
  loop: `if (s <= 0) { -inf } else { loop without the branch }`. Verified by
  hand: `g7_control_hoisted_by_hand.stan` runs in 1.5 µs, against 9.3 µs for
  the map and 0.98 µs with no guard. The `if` on a parameter becomes one
  island, and its compiler already folds the arm's loop into one vector call
  (`CALL OP_NORMAL_LPDF(r5[len 434], r439[len 434], r1[len 1])` in
  `dump_islands`). So the change is a lowering rewrite before the map
  selection: when the condition of a branch in the body reads neither the
  loop variable nor any name that the body assigns (after UDF arguments are
  bound at the call site), split the loop into one loop per arm under a
  single `if`. It is safe only when the branch is reached on every iteration
  with the same inputs, which holds for a top-level guard in a body whose
  iterations are independent. Each hoisted condition doubles the code that
  is lowered, so the number of hoisted conditions needs a small cap.
  Computing both arms and selecting one is not an option here, because
  `normal_lpdf` throws for `s <= 0`, which is the case that the guard
  exists for.

## Reproducer files

All in `loop-fallback/`:

| file | data | gate |
| --- | --- | --- |
| `g1_base.stan`, `g1_assign_outer.stan`, `g1_continue.stan` | `rows.json`, `rows2000.json` | A |
| `g1_heavier_base.stan`, `g1_heavier_assign_outer.stan` | `rows2000.json` | A |
| `g2_nested_ragged.stan`, `g2_control_constant_inner_bound.stan`, `g2_control_outer_unrolled_by_hand.stan` | `groups.json`; `groups40.json` and `rows224.json` for 40 groups | B |
| `g3_base.stan`, `g3_dead_other_elem.stan`, `g3_live_other_elem.stan`, `g3_control_dead_own_elem.stan` | `radon_tiny.json`, `radon_big.json` | C |
| `g4_inner_data_loop.stan`, `g4_control_flat_array.stan` | `inner_small.json`, `inner_mid.json`, `inner_big.json` | D |
| `g4b_inner_sum_vector.stan`, `g4c_inner_sum_local.stan` | `inner_sum.json`, `inner_sum8k.json` | E |
| `g5_reversed_store.stan`, `g5_control_forward_store.stan` | `rows400.json` | F |
| `g6_invariant_reject.stan`, `g6_invariant_reject_corpus_cond.stan`, `g6_control_no_reject.stan`, `g6_control_if_without_reject.stan`, `g6_control_if_without_reject_corpus_cond.stan` | `rows.json` | G |
| `g7_guard_on_scale.stan`, `g7_guard_on_mu.stan`, `g7_no_guard.stan`, `g7_control_hoisted_by_hand.stan` | `rows434.json` | H |
| `dm.sh`, `bg.sh` | | helpers |
