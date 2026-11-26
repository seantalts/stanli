# Root causes of the unexplained cliffs

Worktree `the perf/perturbation-fuzz worktree` at `b046d6169`, tools from
`build-rel`. Nothing in the worktree was edited or rebuilt. MIR came from the
harness cache (`variants-run/cache/mir`) or from the same embedded compiler
with `--vectorize-loops on` (`rootcause/unexplained/mir.sh`).

Each item says what I **ran** and what I only **read**. "Profile" means
`STANLI_PROFILE=1 bench_grad MODEL DATA N`, which prints forward and backward
time per opcode. Timings are `bench_grad --timed`, 3 points, one process.
`rootcause/unexplained/run_all.sh` times every reproducer pair; its output from
this session is quoted under each item.

Verdicts: (a) a real cliff with a cause in a pass or a kernel; (b) the base
reaches a special fast path that the variant loses; (c) not a fair comparison.

## Summary

| item | verdict | cause | change |
|---|---|---|---|
| 1 ch12 vector_eta, probit_vector | b | base: CSE merges 9,930 equal scalar terms into 7 (`cse.cpp:210-221`); one vector density op has nothing for CSE to merge; the only row collapse runs in fast mode only (`lower.cpp:1029`), and there `outcomes_of` refuses any density with an idata header, which ordered_logistic has (`collapse.cpp:192-197`); ordered_probit is not a candidate at all | a default-mode row collapse would close it, but the collapse design note says "Default mode is untouched" on purpose; at least let fast mode read ordered_logistic's outcomes past the header |
| 1 ch12 categorical_loop | b | categorical's outcome is a data slot, not an immediate (`function_registry.cpp:879`, `integer_args = 0`); CSE keys inputs by slot id (`cse.cpp:216-221`), so equal outcomes never merge. Partition also declines the 9,930 lanes ("lane widths do not agree", `partition.cpp:900-910`) because all lanes share one 7-vector p | key constant data slots by value in CSE (gives the base's speed); a partition fix would give one vector categorical, which is faster but not a collapse |
| 2 radon C13 x5 x2 | a | `gather_bwd` (`elementwise.cpp:323-345`) sums runs of equal indices; the 2-vector spellings gather pairs `[2c, 2c+1, ...]` or `[c, c+J, ...]`, so every run has length 1, and with rows sorted by group the same two slots are updated over and over, each update waiting for the one two values earlier; above 512 slots `scatter_add_dense` keeps one sum per slot (`pairwise_sum.hpp:204`) | find runs at a fixed period K in `scatter_add_dense`, or lower `Z[n] * col(ab, g[n])` for small K as K separate gathers and a multiply-add |
| 3 sw_mixture array_log_mix | a | `OP_LOG_MIX_VEC` runs through the generic Stan Math wrapper: nested var tape in forward (active-forward flag, `builtin_registry.cpp:1443`) and again in backward (`library_fns.cpp:51-66`); 420 ns per 2-element call | native double kernel for `log_mix(theta, lambda)` with its closed-form gradient, or lower it to `log_sum_exp(log(theta) + lambda)` |
| 4 C15 udf_early_return (4 cliffs) | a, but a known and accepted limit | a branch on a parameter inside the loop body sends the loop to `lower_region_map` (gate at `lower_region_map.cpp:394-396`, `ParameterControl` at `:50-72`); the region runs ~15 island instructions per row instead of one fused vector normal_lpdf | the project treats slow parameter conditionals as acceptable; the fix would be loop unswitching plus graph arms for a parameter `if`, which I do not recommend as work |
| 5 nhanes mu_loop_reversed | a | reroll requires a positive index stride for element stores (`reroll.cpp:964` and `reroll.cpp:1268-1274`); a loop from N down to 1 has stride -1, so the loop stays scalar and the island carver moves it into the island (21 -> 176 instructions) | accept a constant negative stride (reverse the lane order) when no lane reads another lane's output |
| 5 s2_cens_interval loop_right_f50 | a (kernel) | `OP_NORMAL_LCCDF` costs 98 ns forward per scalar call against 61 ns for the same `normal_lcdf(-y | -mu, s)` (measured); the likely cause (read) is `reflected_cdf` (`densities_cdf_b.cpp:16-30`), which copies the KernelCtx and heap-allocates two std::vectors per call; Stan Math's own normal_lccdf is the same reflected normal_lcdf | negate the scalars in the lambda, or use stack storage for short inputs |
| 5 s2_cox_cens loop_f10 | a | the data puts a censored row every 10th row; reroll takes the period of 10 rows (`reroll.cpp:939-948`) and builds 10 groups of 4 lanes with stride-10 gathers, so the 9 identical uncensored groups never merge and partition (which exists for branches of a data condition) gets nothing | prefer partition's bucketing when a period is made of shorter blocks that match, or merge vector groups with the same template after reroll |
| 6 hmm C18__hmm_marginal x3 | a | `OP_HMM_MARGINAL` uses the same generic wrapper: forward evaluates `hmm_marginal` on a nested var tape (`builtin_registry.cpp:1453`), backward evaluates it again on a new tape (`library_fns.cpp:63-66`). Forward and backward each cost about as much as the whole island | evaluate once in forward and keep the input gradients in scratch (the output is a scalar), then scale them by the output adjoint in backward |
| 7 blr D200, arK K40 | c | the variants do 40x (blr) and 6.6x (arK) more arithmetic; stanli grows about linearly or less, CmdStan grows less because its per-gradient overhead dominates the small bases; stanli stays 1.4x to 6.4x faster than CmdStan on every variant | none needed for the cliff; optional: rewrite `rows_dot_product(X, rep_matrix(beta', N))` to `X * beta` |
| 8 gp C30__matern32 x4 | a | `gp_cov_bwd` has a closed-form backward only for exponentiated quadratic (`matrix_fns.cpp:288`); Matern 3/2 goes to `gp_cov_replay_bwd`, which recomputes the matrix on a nested Stan Math tape: 2.95 us against 0.12 us per gradient, the same in all 4 variants | add a closed-form backward for Matern 3/2 (and 5/2, exponential); OPTIMIZATIONS.md says these kernels stay on Stan Math's tape to keep its reverse order for aliased diagonal entries, so the new branch needs the 10-ULP check |

## 1. ch12_m12_4: vector_eta, probit_vector, categorical_loop

**Ran.** `dump_ops` of base, `C14__vector_eta`, `C14__probit_vector`,
`C14__categorical_loop`; `bench_grad` with and without `--fast-math`;
`STANLI_COLLAPSE_REPORT=1 bench_grad --prep --fast-math`.

The base does collapse the rows into counts per category, but not in the
collapse pass. Its 9,930 scalar `ORDERED_LOGISTIC_LPMF` ops differ only in the
outcome, which lives in the op's immediates (`idata=[4,6,1,0]`), so CSE gives
equal outcomes one key and merges them. Its rule for whole-number seeds
(`cse.cpp`, header comment, and `scale_repeated_terms`) turns k copies of a
term into the term times k. The final base program is 7 `ORDERED_LOGISTIC_LPMF`
ops, 7 `MUL`s by the counts and two `ADD_N`s: 690 ns per gradient.

`R ~ ordered_logistic(rep_vector(0, N), c)` lowers to one op over 9,930
elements (`in=s6(l9930),s2(l6)`). CSE finds one op and has nothing to merge.
The only pass that collapses a vector density by distinct rows,
`collapse_observations`, runs only in fast mode (`lower.cpp:1029`). Even
there it refuses this op with "argument shapes are not per observation" and
`"n":1`. The refusal comes before the length check: `outcomes_of` returns
`ok = false` for every density whose outcomes are followed by an idata header
(`collapse.cpp:192-197`), and ordered_logistic has a 3-word header
(`outcome_idata_header`, `optable.hpp:867`). The cutpoints (length 6) would
then also fail the check that every argument has length 1 or n
(`collapse.cpp:1164-1169`). The probit variant gets no COLLAPSE line at all:
ordered_probit is not a density that the pass considers. Fast mode gave the
same 440 us and 820 us.

The collapse design note
(`notes/performance/2026-10-06-sufficient-statistic-collapse.md`, line 26)
says "Default mode is untouched". Its reason is that a collapsed term reports
errors and rounds differently from default mode (lines 223-229). So the
default-mode change below goes against a recorded decision; CSE already does
the same weighting for identical scalar terms in default mode, which is the
argument for it. The vector kernel
itself is fine: the report gives stanli 1.02x (logistic) and 1.11x (probit)
faster than CmdStan on the variants. So the gap is the lost fast path, and
CmdStan's base is slow because it makes 9,930 scalar calls.

`C14__categorical_loop` builds the same probabilities by hand and writes
`R[i] ~ categorical(p)`. Its 9,930 `CATEGORICAL` ops each read their outcome
from a different data slot (`in=s55(l1),s9(l7)`, `s57`, `s59`, ...), because
the categorical density spec has `integer_args = 0`
(`function_registry.cpp:879`). CSE keys each input by slot id and version
(`cse.cpp:216-221`), not by value, so rows with the same outcome never get the
same key. The variant runs at 180 us, still 2x faster than CmdStan.

The lane report also shows a decline that the harness did not count as new,
because the base has the same reason text:
`partition ... "lanes":9930, "opcode":"OP_CATEGORICAL", "reason":"lane widths do not agree"`.
Partition fuses a categorical only when each lane has its own probability
row (`partition.cpp:931-944`); here every lane reads the same 7-vector `p`,
which partition treats as a shared wide input and refuses
(`partition.cpp:900-910`). Fixing that would give one vector categorical over
9,930 outcomes, which is faster than 9,930 scalar ops but still far from the
base's 7 terms. The CSE change is the one that gives the base's speed back.

Reproducers (`unexplained/ordered`, `unexplained/collapse_categorical`, N = 2000,
4 categories):

```
ordered               loop              380 ns   11 ops
ordered               vector          92238 ns    4 ops
collapse_categorical  ordered_loop      385 ns   11 ops
collapse_categorical  categorical_loop 37339 ns 2428 ops
```

```stan
data { int<lower=1> N; array[N] int<lower=1, upper=4> R; }
parameters { ordered[3] c; }
model {
  c ~ normal(0, 1.5);
  R ~ ordered_logistic(rep_vector(0, N), c);   // loop form: 380 ns
}
```

**Change.** In fast mode at least: let `outcomes_of` read the outcomes before
ordered_logistic's header, and let an argument that every observation shares
(the cutpoints) pass the shape check. Doing the same in default mode would
need the decision in the design note to be revisited. For categorical, key
constant data slots by value in CSE, or carry the outcome in idata as
ordered_logistic does.

## 2. radon_variable_intercept_slope_{centered,noncentered} / C13

**Ran.** `dump_ops` and profiles of base and all five spellings (colfn and
vec_array centered; row_dot and colslice for both; rowfn noncentered); a
reproducer with J = 200 and J = 300, sorted and shuffled groups.

All five spellings give the same program shape: the two group effects of each
row gathered into one interleaved vector, then `GROUP_DOT` with groups of 2.
The column spellings gather `[2c, 2c+1, ...]`; the row spellings gather
`[c, c+386, ...]`. In every one the large gather's backward costs about
43 us per gradient.

```
base:   GATHER a (12573) ; GATHER b (12573) ; MUL ; ADD ; NORMAL_LPDF
colfn:  CONCAT2 ; GATHER (772, interleave) ; GATHER (25146, idata [16,17,16,17,...]) ; GROUP_DOT(K=2) ; NORMAL_LPDF
```

Per gradient, from the profile (2000 gradients):

| op | base | colfn |
|---|---|---|
| GATHER backward | 12.4 us (two ops) | 43.5 us (one op) |
| GROUP_DOT, or MUL + ADD | 15.9 us | 17.0 us |
| total | 58.5 us | 89.6 us |

The whole difference is the backward pass of the large gather. `gather_bwd`
(`elementwise.cpp:323-345`) has 32 adjoints or more per source element here,
so it calls the compensated `scatter_add`, which goes to `scatter_add_dense`.
That code sums runs of equal indices in registers. The base indices are
sorted by county (`[8,8,8,8,...]`), so the runs are long. In the 2-vector
spellings the indices alternate between two slots, so every run has length 1
and every value goes through a two-sum in memory. Because the rows are sorted
by county, the same two slots receive value after value, and each update has
to wait for the update two values earlier. With 772 slots (more than
`kScatterLaneSlotsMax = 512`, `pairwise_sum.hpp:204`) the code keeps one sum
per slot instead of four, so nothing hides that wait.

The reproducer (`unexplained/pair_gather`, N = 12,000) shows all three parts.
GATHER backward is per gradient:

```
split  J=200 sorted    46299 ns   GATHER bwd  8.8 us
pairs  J=200 sorted    70502 ns   GATHER bwd 22.3 us   (400 slots: four sums per slot)
split  J=300 sorted    47733 ns   GATHER bwd 10.2 us
pairs  J=300 sorted    89177 ns   GATHER bwd 40.6 us   (600 slots: one sum per slot)
split  J=300 shuffled  52337 ns   GATHER bwd 13.6 us
pairs  J=300 shuffled  69787 ns   GATHER bwd 20.8 us
```

With shuffled groups the 2-vector form is still 1.33x slower, and its gather
backward is twice as fast as with sorted groups. So the sorted base gains a
little from its runs, but most of the cliff is the 2-vector form's index
pattern meeting sorted data: verdict (a).

```stan
data { int<lower=0> N; int<lower=0> J; array[N] int<lower=1, upper=J> g;
       vector[N] x; vector[N] y; }
transformed data { matrix[N, 2] Z = append_col(rep_vector(1, N), x); }
parameters { vector[J] a; vector[J] b; real<lower=0> sigma; }
model {
  a ~ normal(0, 1); b ~ normal(0, 1); sigma ~ normal(0, 1);
  matrix[2, J] ab = append_row(a', b');
  vector[N] mu;
  for (n in 1:N) mu[n] = Z[n] * col(ab, g[n]);  // a[g[n]] + x[n] * b[g[n]] is 1.5x to 1.9x faster
  y ~ normal(mu, sigma);
}
```

**Change.** Either make `scatter_add_dense` find runs at a fixed period K
(index k and index k + K name the same slot, as in a gather of K-vectors), or
lower a `GROUP_DOT` over a gather of K-vectors with small K as K separate
gathers, one per position, and a multiply-add, which is the base's program.

## 3. sw_mixture / C4__array_log_mix

**Ran.** `dump_ops`, profiles of base and variant; reproducer.

The variant swaps `log_sum_exp` of `log(theta_k) + normal_lpdf` for
`log_mix(theta, ps)`. Neither program is rerolled (40 scalar rows each). The
profile per call:

| op | forward | backward |
|---|---|---|
| LOG_SUM_EXP (base) | 21 ns | 15 ns |
| LOG_MIX_VEC (variant) | 201 ns | 218 ns |

`OP_LOG_MIX_VEC` is registered with `register_library<LogMixVecOp>`
(`library_fns.cpp:426`). The builtin map sets `kMatrixActiveForward` for
log_mix (`builtin_registry.cpp:1443`), so `library_fwd` builds a nested var
tape and evaluates Stan Math's `log_mix` with vars (`library_fns.cpp:51-61`),
and `library_bwd` builds another tape and evaluates it again (`nary_bwd`,
`legacy.hpp:54-80`). Each call also allocates `std::vector<Eigen::Vector>`s.
For a 2-element mixture that is about 10x the cost of the base's ops.
(Read only: why the forward needs vars. The comment at
`builtin_registry.hpp:474` says that the flag keeps the value equal to Stan
Math's var overload.)

Reproducer (`unexplained/log_mix`, N = 40): `lse` 4386 ns, `log_mix` 18730 ns.

```stan
data { int<lower=1> N; vector[N] y; }
parameters { simplex[2] theta; ordered[2] mu; real<lower=0> sigma; }
model {
  mu ~ normal(0, 5); sigma ~ normal(0, 2);
  for (n in 1:N) {
    array[2] real ps;
    ps[1] = normal_lpdf(y[n] | mu[1], sigma);
    ps[2] = normal_lpdf(y[n] | mu[2], sigma);
    target += log_mix(theta, ps);   // log_sum_exp(log(theta[k]) + ps[k]) is 4.3x faster
  }
}
```

**Change.** A native kernel for `log_mix(theta, lambda)` on doubles with the
closed-form gradient (weights `w_k = theta_k exp(lambda_k - lp)`; d/d theta_k =
`exp(lambda_k - lp)`, d/d lambda_k = `w_k`), checked against Stan Math to 10 ULP;
or lower `log_mix(theta, lambda)` to `log_sum_exp(log(theta) + lambda)`, which
the existing kernels and passes handle.

## 4. C15__udf_early_return (aalto_lin 1 and 2, kidscore_momiq, s2_car)

**Ran.** `dump_ops`, `dump_islands`, profile of aalto_lin base and
`C15__udf_early_return1`; reproducer with the guard inside the loop and with
the guard hoisted by hand.

The base has one fused `NORMAL_LPDF` over all rows. The variant calls

```stan
real guarded_lpdf(real y, real mu, real s) {
  if (s <= 0) return negative_infinity();
  return normal_lpdf(y | mu, s);
}
```

per row. `s` is `sigma`, a parameter, so the loop's control flow depends on a
parameter. `lower_region_map` (called at `lower_stmt.cpp:2403`) accepts a loop
only when `loop_has_parameter_control` is true (`lower_region_map.cpp:394-396`,
which uses `ParameterControl` at `:50-72`), and this loop passes that test. The region runs the body
as an island program for each row: 15 forward instructions and 10 adjoint
instructions, with one scalar `NORMAL_LPDF` call per row. On aalto_lin (N = 8)
the `REGION_MAP` costs 245 ns forward and 264 ns backward per gradient against
about 37 ns for the fused density. No pass reports a decline, because the
loop never reaches reroll.

The condition reads only `sigma`, which the loop never writes, so it has the
same value in every row. It could be moved out of the loop. I tried this by
hand (`guard_hoisted.stan`): the program becomes one `ISLAND` whose else arm
holds 140 unrolled scalar instructions, which is better (559 ns) but still
2.7x slower than the base (207 ns), because an island arm cannot hold the
fused vector density.

Reproducer (`unexplained/udf_guard`, N = 64):

```
base           207 ns   NORMAL_LPDF over 64 rows
guard_loop    1646 ns   REGION_MAP
guard_hoisted  559 ns   ISLAND with the loop unrolled inside
```

```stan
functions {
  real guarded_lpdf(real y, real mu, real s) {
    if (s <= 0) return negative_infinity();
    return normal_lpdf(y | mu, s);
  }
}
data { int<lower=0> N; vector[N] x; vector[N] y; }
parameters { real alpha; real beta; real<lower=0> sigma; }
model {
  alpha ~ normal(0, 1); beta ~ normal(0, 1); sigma ~ normal(0, 1);
  for (n in 1:N) target += guarded_lpdf(y[n] | alpha + beta * x[n], sigma);
}
```

**Change.** The cause is a conditional on a parameter, and the project has
decided that such conditionals may stay slow as long as they compile (user
guidance recorded 2026-08-09: simplicity over speed, parameter conditionals
are rare). So I list this as a known limit, not as recommended work. For the
record, a fix would need two steps: move a condition that reads only values
the loop does not write out of the loop (loop unswitching), and lower a
top-level `if` on a scalar parameter so that each arm stays an ordinary op
graph run conditionally, not an island program. Evaluating both arms and
selecting is not correct, because `normal_lpdf` rejects when `s <= 0`.
`early_return2` adds `is_nan(mu)`, which depends on the row, so unswitching
would handle only its first guard. On the variants, stanli is 1.53x
(kidscore_momiq) and 1.45x (s2_car) faster than CmdStan, and 1.44x and 1.98x
slower than CmdStan on aalto_lin (N = 8), from the results records.

## 5a. i320_mi_nhanes / C8__mu_loop_reversed

**Ran.** `dump_ops`, `dump_islands`, profile, `STANLI_LANE_REPORT=1`,
`stanli_check --dump-passes=all` for base and variant; reproducer.

The variant writes `mu_bmi[n] += ...` for `n` from `N_bmi` down to 1. After
constant folding the store chain uses indices 24, 23, 22, ... (base: 0, 1, 2,
...). In the base, reroll shrinks the log_prob graph from 333 to 64 lines; in
the variant it removes one line. Both the store test and the marching test
require a positive stride: `if (stride >= 1) marching.push_back(...)`
(`reroll.cpp:964`) and `int64_t br_run = stride >= 1 ? Luse : 0;`
(`reroll.cpp:1268-1274`). The island carver then takes the scalar ops into
the island, which grows from 21 to 176 instructions and from 120 ns to 727 ns
per gradient.

Reroll does report the decline: the variant has four new
`"a store does not march through one private vector"` lines. The harness did
not count them as new, because the base already has that reason text at other
ops, and `new_declines` compares only the reason strings. The same thing hid
the partition decline in item 1 (categorical). Any cliff in this group whose
new decline repeats a reason that the base already has is affected.

Reproducer (`unexplained/reversed`, N = 25): `forward` 168 ns (12 ops),
`reversed` 453 ns (6 ops; `dump_islands` shows one island of 100 forward
instructions).

```stan
data { int<lower=1> N; vector[N] x; vector[N] y; }
parameters { real a; real b; real<lower=0> sigma; }
model {
  vector[N] mu = rep_vector(a, N);
  for (i in 1:N) { int n = N - i + 1; mu[n] += b * x[n]; }   // for (n in 1:N) is 2.7x faster
  y ~ normal(mu, sigma);
}
```

**Change.** In reroll, accept a constant negative stride for element stores
and element reads when no lane reads a value that another lane wrote, by
reversing the lane order (lane l becomes lane L-1-l). The harness could also
compare decline records by op and reason, not only by reason text.

## 5b. s2_cens_interval / C5__loop_right_f50

**Ran.** Profiles of the base model with the base data and with the f50 data;
reproducer.

This is a change of data only: base data has 30 uncensored rows and 10 interval
rows (`normal_lcdf` twice plus `log_diff_exp`); f50 has 20 uncensored and 20
right-censored rows (`normal_lccdf`). Both keep their cdf terms as 20 scalar
ops. The work is comparable, and CmdStan takes the same time on both. The
difference is the per-call cost of the cdf kernels:

| op | forward per call |
|---|---|
| NORMAL_LCDF | 59 ns |
| NORMAL_LCCDF | 101 ns |

The math is the same: Stan Math's own `normal_lccdf` returns
`normal_lcdf<normal_lccdf_func>(-y, -mu, sigma)`
(`deps/math/stan/math/prim/prob/normal_lccdf.hpp:18`), where the template
argument only names the function in error messages. What differs is stanli's
wrapper (read, not measured on its own): `normal_lccdf` goes through
`reflected_cdf` (`densities_cdf_b.cpp:16-30`), which copies the whole
`KernelCtx` and fills two heap-allocated `std::vector<double>` with the
negated inputs on every call. That is the likely cause of the extra 37 ns.

Reproducer (`unexplained/lccdf`, N = 20, same math both ways):
`normal_lcdf(-y[n] | -mu, sigma)` 1164 ns, `normal_lccdf(y[n] | mu, sigma)`
1828 ns. Profile: 61 ns against 98 ns forward per call.

**Change.** Negate the scalars inside the lambda (or into stack storage when
the inputs are short) instead of allocating. This affects
`OP_NORMAL_LCCDF` and `OP_STD_NORMAL_LCCDF` (`optable.hpp:457-459`). It is a
small kernel cost, but it is 1.7x per call.

## 5c. s2_cox_cens / C5__loop_f10

**Ran.** `dump_ops`, lane report and timing of `C5__loop_f10` with its own
data and with the censoring flags sorted, moved to the end, and shuffled;
reproducer.

The ops did grow here (33 to 109), but no pass reported a decline. The f10
data marks rows 0, 10, 20, 30 as censored. With that period in the data, the
unrolled loop body repeats every 10 rows, and reroll takes that period
(`reroll.cpp:939-948`): it builds one group of 4 lanes for each of the 10 row
positions, each with its own stride-10 gathers (`idata=[1,11,21,31]`,
`[2,12,22,32]`, ...). The 9 uncensored groups have the same template but stay
separate. Partition groups lanes by the shape of their ops so that the two
arms of a data condition can share a bucket (`partition.cpp` header), but it
runs after reroll and finds no scalar lanes left to group.

Same source and same values, only the order of the flags:

```
C5__loop_f10.json (period 10)  109 ops  1607 ns
censored rows first             27 ops   956 ns
censored rows last              27 ops   985 ns
shuffled                        90 ops  1305 ns   (partition: "lanes read different non-constant slots")
```

Reproducer (`unexplained/periodic_branch`, N = 40): sorted flags 25 ops
344 ns, period-10 flags 74 ops 758 ns.

```stan
data { int<lower=1> N; vector[N] x; vector[N] bhaz; vector[N] cbhaz;
       array[N] int<lower=0, upper=1> cens; }
parameters { real a; real b; }
model {
  vector[N] log_mu = a + x * b;
  a ~ normal(0, 1); b ~ normal(0, 1);
  for (n in 1:N) {
    if (cens[n] == 0)
      target += log(bhaz[n]) + log_mu[n] - cbhaz[n] * exp(log_mu[n]);
    else
      target += -cbhaz[n] * exp(log_mu[n]);
  }
}
```

**Change.** When the period that reroll finds is made of shorter blocks with
the same template (9 of the 10 rows here), leave those ops to partition, or
merge vector groups with the same template after reroll by joining their
gathers. The shuffled case shows a second limit, in partition, which I did
not trace.

## 6. hmm_example, hmm_gaussian, hmm_drive_0 / C18__hmm_marginal

**Ran.** `dump_ops` and profiles of base and variant for all three seeds;
reproducer.

On hmm_example stanli's `hmm_marginal` is 1.7x slower than its island for
the hand-written loop; on the other two it is about as fast as the island. In
all three it evaluates `hmm_marginal` on the var tape twice, so it misses the
speedup that CmdStan gets from the builtin. Per gradient:

| seed | island (base), fwd + bwd | HMM_MARGINAL, fwd + bwd | CmdStan variant |
|---|---|---|---|
| hmm_example (N=100, K=2) | 8.8 + 9.6 us | 15.4 + 15.7 us | 19.1 us |
| hmm_gaussian (T=500, K=3) | 92 + 119 us | 88 + 90 us | 116 us |
| hmm_drive_0 (N=416, K=2) | 53 + 71 us | 59 + 60 us | 89 us |

`OP_HMM_MARGINAL` is `register_library<HmmMarginalOp>`
(`library_fns.cpp:247-254`, `:427`) with `kMatrixActiveForward`
(`builtin_registry.cpp:1453`). Forward builds a nested tape and runs Stan
Math's var `hmm_marginal`, which already computes the gradients. It keeps only
the value. Backward builds a new tape and runs the same function again, then
calls `grad`. Each half costs 66% to 80% of CmdStan's time for the whole
gradient. CmdStan runs it once, which is why it gets 1.5x to 2.5x faster with
the builtin while stanli does not.

Reproducer (`unexplained/hmm`, N = 100, K = 2): `forward` (hand-written)
18890 ns, `hmm_marginal` 33549 ns.

**Change.** The output is a scalar, so forward can run the var evaluation
once, call `grad` with seed 1, and keep the input gradients in scratch;
backward then adds `out_adj` times them. Seeding with 1 and then scaling can
round differently from seeding with `out_adj`, so the change needs the usual
10-ULP check against CmdStan. The same pattern would help every
`register_library` op with a scalar output, including `log_mix` (item 3).

## 7. blr / C26__base_form_D200, C26__rows_dot_rep_D200; arK / C17__lag40, C23__base_loop_K40

**Ran.** Profiles of blr base, base source with D=200 data, and
`rows_dot_rep_D200`; profiles of arK with K=5 and K=40.

These are not fair comparisons, and stanli is faster than CmdStan on every
variant.

- **blr base_form_D200.** Same source, X goes from 100 x 5 to 100 x 200: 40x
  the arithmetic. stanli's `MATVEC` goes from 0.20 us to 3.6 us per gradient
  (18x), and the gradient from 365 ns to 4.3 us (11x). CmdStan goes from about
  1.0 us to 5.8 us (5.75x), because its fixed cost per gradient dominates at
  D=5. On the variant stanli is 1.43x faster than CmdStan.
- **blr rows_dot_rep_D200.** `rows_dot_product(X, rep_matrix(beta', N))` builds
  a 100 x 200 copy of beta (`REP_MAT`, 8 us per gradient) and a `GROUP_DOT`
  over it (21 us). That is 6.7x slower than `X * beta` at D=200, but CmdStan
  is 7.6x slower with the same spelling. stanli is 1.5x faster than CmdStan on it.
  The 84x figure divides by the tiny `X * beta` base at D=5. An optional
  improvement is to rewrite `rows_dot_product(X, rep_matrix(v', N))` to
  `X * v'`.
- **arK lag40 / base_loop_K40.** Same source with K=40 lags in the data instead
  of 5. Each lag becomes one vector `MUL` and one vector `ADD` over the rows,
  so the ops grow from 21 to 126 by design. The work grows 6.6x
  ((200-40) x 40 against (200-5) x 5 multiply-adds); stanli's gradient grows
  6.7x, CmdStan's 4.55x. stanli stays 6.4x faster than CmdStan (9.6x on the
  base). An optional improvement is to fold the K axpy ops into one
  lag-matrix product, which would save memory traffic for large K. That is
  not a cliff.

## 8. gp_regr, gp_pois_regr / C30__matern32_*

**Ran.** Profiles of base, `C30__matern32_loop` and `C30__matern32_add_diag`
for both gp_regr and gp_pois_regr, and of gp_regr `C30__add_diag`
(exp-quad with add_diag); reproducer. Read: `gp_cov_bwd`.

Yes. The only difference in the program is `GP_COV v=00` against `v=01`
(the `_loop` variants also have the jitter folded into GP_COV as a fourth
input). Forward is the same (0.5 us). Backward, the same in all four variants:

| GP_COV backward per gradient | |
|---|---|
| exponentiated quadratic | 0.12 us |
| Matern 3/2 | 2.95 us |

`gp_cov_bwd` (`matrix_fns.cpp:281-290`) has a blocked closed-form backward
that follows Stan Math's fixed-location exp-quad callback, but only when
`mode.family == kGpExpQuad` (and there is no jitter input, locations are data,
and the scales are normal numbers). Every other family goes to
`gp_cov_replay_bwd` (`matrix_fns.cpp:211`), which builds a nested var tape,
recomputes the whole N x N covariance with vars, and calls `grad`. This
also explains why CmdStan slows less: Stan Math's Matern 3/2 is a plain var
expression in both systems, while stanli's exp-quad base is unusually fast.

Reproducer (`unexplained/matern`, N = 11): `exp_quad` 2505 ns, `matern32`
5412 ns.

```stan
data { int<lower=1> N; array[N] real x; vector[N] y; }
parameters { real<lower=0> rho; real<lower=0> alpha; real<lower=0> sigma; }
model {
  matrix[N, N] L = cholesky_decompose(add_diag(gp_matern32_cov(x, alpha, rho), sigma));
  rho ~ gamma(25, 4); alpha ~ normal(0, 2); sigma ~ normal(0, 1);
  y ~ multi_normal_cholesky(rep_vector(0, N), L);
}
```

**Change.** Add closed-form backward branches for the other stationary
kernels when the locations are data. For Matern 3/2 with `d = sqrt(3) r / rho`:
`k = alpha^2 (1 + d) exp(-d)`, `dk/d alpha = 2 k / alpha`,
`dk/d rho = alpha^2 d^2 exp(-d) / rho`. Matern 5/2 and the exponential kernel
work the same way. `runtime/src/OPTIMIZATIONS.md` (the section on folding
diagonal updates into GP covariances, around line 155) records why these
kernels stay on Stan Math's tape: some of them return diagonal entries that
alias one another in Stan Math, and the tape keeps their reverse accumulation
order. A closed-form branch changes that rounding, so it needs the 10-ULP
check against CmdStan, with the fused jitter case included. I found no record
that a closed-form Matern backward was tried and rejected.

## Limits of this evidence

- All timings are single short runs on a shared machine (other work was
  timing). The ratios are large enough that noise does not change a verdict,
  but the absolute numbers are not benchmark quality.
- Profiles add overhead to every op. I used them only to compare ops within
  one program and across matched programs.
- Read, not run: the claim that a default-mode row collapse or a CSE that
  keys data by value would bring the vector and categorical forms back to
  about 700 ns; the effect of each proposed change; the reason that the
  library ops need an active forward.
- Not traced: why partition declines the shuffled cox data ("lanes read
  different non-constant slots").
- Prior records: I searched `runtime/src/OPTIMIZATIONS.md`,
  `docs/internal/research/` and `notes/` for matern, hmm_marginal, log_mix,
  lccdf, descending or negative strides, unswitching and
  `kScatterLaneSlotsMax`. The only relevant records are the GP note in
  item 8 and the collapse design note in item 1. Neither rejects the changes
  that are proposed here.
