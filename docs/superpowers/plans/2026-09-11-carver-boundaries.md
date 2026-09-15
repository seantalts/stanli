# Carver boundaries: one cost function for island, split, and leave

## 1. What "strict" means today

`in_vocab(g, op, strict)` (island.cpp:180) is the carver's structural vocabulary
test. `strict` changes exactly two branches; everything else it refuses is
refused under both vocabularies.

| predicate | line | excludes | status |
| --- | --- | --- | --- |
| binary/ternary elementwise, strict branch | 194 | vector operands | historical: `compile_elementwise` (382-406) emits RANGE/EXP_RANGE/LOG_RANGE at any width since 4aeb267e widened the non-strict vocabulary |
| scalar unaries, strict branch | 210-214 | vector width but LOGV/EXPV | historical, same reason; 178-179 calls strict "the vocabulary before elementwise ops had range forms" |
| OP_ADD_N | 196 | vector ADD_N, always | correctness: `compile()`'s ADD_N case (425-436) emits scalar ADD/MOV, no width/broadcast handling |
| callable() vector-out | 171 | output len != 1 | scope, not a machine limit: unimplemented per 2026-08-09-kernel-call-instruction.md phase 2 |
| callable() meta/effectful | 158-165 | OP_ISLAND/ODE/DAE/ODE_ADJOINT/RNG/PRINT/REJECT | correctness: not pure register arithmetic |
| propto densities | 218-219 | variant & 0x80 | correctness: term-dropping needs argument TYPES; islands bind every argument as T |

So `strict` marks two op classes the *old* machine could not run at vector
width, fine on the current one; its only job left is telling `split_cost`
(930-955), `split_cost_floor` (901-913), and `split_has_multiple_pieces`
(915-924) where to cut, no present correctness meaning.

## 2. A liveness-based boundary

Today a run ends, or a split cuts, wherever `in_vocab(g, op, strict)` first
fails: a structural test blind to what crosses that point. A liveness-based
boundary asks instead: inside an eligible run `[i, j)` (`in_vocab(.., false)`,
the real vocabulary), where is a cut `k` cheap, meaning few values must cross
it?

The carver already computes that quantity twice, in agreement: `evaluate`'s
`c.boundary` (802-803, from a compiled program's live-ins/live-outs) and
`joined_boundary(i, j)` (848-878, the same two terms -- `2 * len` per live-in,
`kOpCost + 3 * len` per live-out -- computed straight from graph slots and
`last_use`, before compiling anything).

For a cut at `k`, the values that must cross are the slots produced in `[i, k)`
and read again at or after `k`: each needs one extraction (`kOpCost + 3 * len`)
and one seed (`2 * len`). That cost is exactly `joined_boundary(i, k) +
joined_boundary(k, j) - joined_boundary(i, j)`: the two pieces' independent
charges minus what the whole span would have paid for the same slots, so no new
concept, just `joined_boundary` applied twice and netted against itself. A
liveness-chosen boundary is a `k` that keeps this small, found by tracking
currently-live producer slots in one pass, not by asking where the historical
vocabulary refuses an op.

## 3. One cost function

    cost(leave, i, j)    = graph_cost(i, j)
    cost(join, i, j)     = island_cost(i, j) + joined_boundary(i, j)
    cost(split, i, j, B) = sum over consecutive pieces P of B of
                             graph_cost(P)                     if |P| < kMinIslandOps
                             min(graph_cost(P), cost(join, P)) otherwise

`B` is a set of candidate cuts inside `[i, j)`: today exactly the strict cut
points, after section 4 also the liveness-chosen ones. The carver's choice is
`argmin` over `{cost(leave), cost(join), min over B of cost(split, B)}`. `leave`
carries no boundary, since nothing crosses a call edge when ops stay inline;
that asymmetry is real. `join` and every piece of `split` share one boundary
term, so a candidate is judged the same way against either alternative. This is
the review's second complaint: `c.accepted` (813) reads `graph_cost >=
island_cost` with no boundary, while `split_wins` (985-993) compares split
against `island_cost + boundary`; the same candidate is judged two ways
depending which alternative sits next to it. Under one function, `c.accepted`
becomes `graph_cost(i, j) >= cost(join, i, j)` everywhere, judged from these
same three numbers, never a boundary-in/boundary-out mix of one candidate.

Surviving as bounds on this function: `join_cost_floor` already adds
`joined_boundary(i, j)` (898), so it is a lower bound on `cost(join)` computed
without compiling the vector ops. `split_cost_floor` already sums
`joined_boundary` over strict sub-runs (901-913), so it is a bound on
`cost(split, B_strict)`, generalized to whatever `B` is in use.
`split_has_multiple_pieces` is a cheap pre-check on `|B| >= 2`, redefined over
the candidate set instead of `grow(.., true)`. `kMinIslandOps` is untouched, an
orthogonal pre-filter on whether a piece is worth costing. Not surviving as the
only source of `B`: the refusal re-carve, meaning "cut wherever the strict
vocabulary refuses inside a non-strict span" (the sole reason `run()` enters the
split path at 1167); the strict vocabulary is no longer a real limit, so it
becomes one candidate boundary among the liveness-chosen ones rather than a
forced cut.

The dispatch weights (`kValueRegWeight = 2`, `kOpCost = 5`, the 8x/17x
multipliers in `graph_op_cost`, 44-80) keep their values and role: named
constants calibrated once from a profile of the interpreter (44-62 already cite
it), documented as a calibration rather than a proof. Recalibrating means a
fixed micro-benchmark of one graph-op dispatch against one island-instruction
dispatch on the current machine, a small `tools/` binary rather than another
corpus sweep.

## 4. Migration, smallest first

Every commit keeps the corpus harness at zero semantic (bitwise-result)
failures: none of this changes which arithmetic a chosen form runs, only which
form is chosen, so the risk is a performance regression, never correctness.

**(a) One boundary for join everywhere -- resolved by recalibration, kept
neither way.** Charging the boundary in `c.accepted` flips `sw_skewnormal`
correctly (it measures 5% slower carved) but `s2_mm`/`s2_mm_weights` measure
1.2x faster despite the same arithmetic saying otherwise, so neither keeping
nor adding the charge was right. Tried recalibrating the constants instead
(a dispatch micro-benchmark, since removed, median of seven runs: graph/island ratio
1.458, live-in 0.131, live-out 0.684, each rounded to 1 with a floor of one):
committed as `2e0580a0`, corpus run (`ab_recal` vs `ab_final9`) showed 46 of
254 models losing their island outright, `iohmm_reg` 1.82x slower per
gradient (prep 16.18x), `hmm_gaussian` 1.46x, `s2_mm`/`s2_mm_weights` 1.16x,
none of that within the 2% keep-or-revert ceiling, so reverted in `3b14b3ee`.
The estimate keeps its current constants and boundary-free acceptance test;
neither a boundary-consistent rule nor these measured constants beat it.

**(b) Floors as bounds.** Rewrite `join_cost_floor`, `split_cost_floor`,
`split_has_multiple_pieces` to visibly bound `cost(join)`/`cost(split)` from
section 3, same arithmetic, no numeric change; assert the floor values are
bit-identical before and after. No carving should change; that is the acceptance
test. Est. 60-100 lines. Risk: low.

**(c) Liveness boundaries alongside strict.** Add a boundary-proposal pass
over an eligible run using section 2's crossing-cost quantity, without removing
strict cuts; price `cost(split, B_strict)` and `cost(split, B_liveness)` and
take the cheaper. `iohmm_reg` is the likely mover: it already takes the `multi`
path with one strict-refusing op inside a 34,959-op span and currently skips
pricing the strict split entirely (`split_floor=233082 joined=218045
split_skip=1`); compare cut positions and cost under both `B`s there before
landing. Est. 150-250 lines (new function, `run()` wiring, hand-built-graph
tests). Risk: medium-high, the first commit adding new search.

**(d) Delete strict splitting where liveness subsumes it.** Once (c) shows
`B_liveness` never worse than `B_strict` on the corpus, remove `strict` from
`in_vocab` (194, 210-214), `grow`'s `strict` parameter,
`strict_queue`/`strict_queue_pos`/`strict_candidate` (615, 620, 970-980), the
rename invalidation around them (961-965, 1264-1270), and the strict-vocabulary
comments (12-22, 175-179). Keep OP_ADD_N's scalar-only restriction, unrelated to
`strict`. A span favoring `B_strict` keeps it as another candidate source
instead. Est. net negative, ~150-200 lines removed. Risk: high, land only after
(c)'s comparison is clean.

## 5. What to delete, what to refuse

Delete: the `strict` distinction and everything built only to reuse strict
sub-run compiles, once (d) lands; the "vocabulary before elementwise ops had
range forms" comment, which will describe nothing.

Refuse, in this migration: widening `in_vocab`'s admitted op set (separate
change); changing `kValueRegWeight`, `kOpCost`, or `graph_op_cost`'s multipliers
(naming and recalibration only, section 3); relaxing the prep-time tuner's
bitwise gate to rescue `sw_skewnormal` or `s2_mm` (already flagged as separate
policy in 2026-09-10-prep-time-tuning.md); touching `reroll.cpp`,
`partition.cpp`, `pass_util.hpp`, or `harnesses/vectorize_ab.py`'s op-count
gate, owned by other agents here; and a boundary charge on `leave` for symmetry,
since leaving ops in place crosses no call edge.
