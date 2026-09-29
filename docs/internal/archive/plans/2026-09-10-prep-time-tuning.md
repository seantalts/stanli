# Prep-time tuning: measure where the estimate cannot decide

## Problem

The island carver chooses between compiling a run of ops into an island,
splitting it, or leaving it interpreted by comparing two cost estimates.
The estimate is a model of the machine. On the corpus it ranks large
islands correctly and mis-ranks small ones both ways: sw_skewnormal's new
island is predicted 7% faster and measures 5% slower; s2_mm's is
predicted 5% faster and measures 12% faster. No margin on the predicted
gain separates the two, because the error is in the estimate, not the
margin. Every rule added to the estimate so far has been tuned on the
corpus and on one machine.

## Principle

The estimate ranks; a measurement decides. Wherever a pass had a real
choice between two forms of the same computation, the runtime keeps enough
to build both, times both on the model's own data, and keeps the faster
one. Nothing in the rule refers to a model, a corpus, or a machine: the
noise threshold is measured, the batch size comes from the clock, and the
budget is a fraction of the time compilation already spent.

## Shape

### Choices

A pass records a `TuningChoice` for each decision it made where the other
option was viable:

    struct TuningChoice {
      std::string what;                       // diagnostics only
      std::function<bool(Graph&)> alternative; // rebuild a copy of the
                                               // graph in the other form
    };

`CompiledModel` carries `std::vector<TuningChoice> choices` for the
log_prob graph. Choices are ordered as the pass met them.

### The island carver's choices

The carver's scan is deterministic: a sequence of candidates, each a span
`[begin, end)` of the pre-island graph with a `strict` flag, and for each
a decision in {island, split, leave}. The carver takes an optional plan:

    struct CarvePlan {
      std::map<CandidateKey, Decision> overrides;   // forced decisions
      std::vector<CandidateRecord> decisions;       // what it did, in order
      CarveCache* cache;                            // compiled candidates
    };

`CandidateKey` is `(begin, end, strict)` in pre-island op indices, which is
stable across replays because the scan reaches the same spans whichever
way a candidate went. `CarveCache` maps a key to the compiled `Candidate`
so a replay never compiles a span twice; `emit` works on a copy.

After the first carve, the carver keeps the pre-island op list it would
otherwise have dropped (`Carver::run` ends by moving `result` over
`g.ops`; the old vector moves into the plan instead of being freed) and
registers one choice per recorded decision that had an alternative:

- island taken, alternative leave;
- join taken, alternative split;
- candidate compiled but refused, alternative island.

`alternative(Graph& g)` rebuilds the pre-island graph into `g` from the
retained ops and the current slots and payload pool (the carver only
appends to those) and replays `carve_islands` with the accumulated
overrides plus this one. The retained ops are released when tuning ends.

### The tuner

`tune(CompiledModel&, PrepTrace&)` in `runtime/src/tune.cpp` runs after
`Lowering::run` for the log_prob graph, before any executor exists. Greedy
over choices in order: build the alternative graph, time it against the
current graph, keep the faster, move on.

Who tunes: `compile_model` runs it unless `STANLI_NO_TUNE` is set. The
test tools that compare structure (`stanli_check`, `dump_ops`, the lit
runner, the harness's op-count and A/B cells) set `STANLI_NO_TUNE` in
their own environment so a graph is a deterministic function of the model
and data there; `bench_grad`, `stanli_run` and the language bindings run
what ships. The harness's cleared-environment list gains `STANLI_NO_TUNE`,
`STANLI_DEBUG_TUNE` and `STANLI_NO_ISLAND_JOIN_GUARD`.

Measurement, for one pair of graphs:

1. Points. Three deterministic unconstrained points: zeros, then two drawn
   uniform in (-2, 2) from a fixed-seed generator, the same distribution
   CmdStan draws inits from. Each graph is evaluated at each point once;
   a point where either graph throws or returns a non-finite lp or gradient
   is dropped. With no usable point, the choice is left as the pass made it.
2. Agreement. At every usable point the two graphs must agree bitwise on
   lp and on every gradient component. If they do not, the choice is left
   as the pass made it and a diagnostic says so under `STANLI_DEBUG_TUNE`.
   A timing decision must never change what a model computes, and this is
   the check that makes the decision numerically invisible: the same seed
   gives the same draws whichever form wins.
3. Batch. A batch is as many gradient evaluations as fit in a target
   duration, at least one. The target comes from the clock: the batch must
   be long enough that the clock's resolution is under one percent of it.
   One evaluation of the current graph sizes the batch for both.
4. Rounds. Up to seven rounds, each timing one batch of each graph in
   alternating order, cycling through the usable points. After three rounds
   the alternative wins if it was faster in every round so far and its
   median batch time is lower; it loses if the current graph was faster in
   every round. Otherwise rounds continue to seven and the alternative wins
   only if it was faster in at least five and has the lower median. A tie
   keeps the pass's choice.
5. Budget. Tuning as a whole may spend at most as long as compilation had
   spent when it started, measured on the same clock. A choice whose first
   round would exceed what remains is left as the pass made it. Choices are
   visited in graph order.

The executor for each graph is a plain `Executor(Graph)` built from a copy;
the timed call is `Executor::gradient`. The parameter vector is written
into `params_data()` before each round.

Results go into the prep trace as a stage `tune` with the number of
choices, flips, rounds and the time spent, so `bench_grad --prep` and the
harness see it.

### Determinism and tests

The choice a model ends up with can differ between runs on the same
machine; its results cannot, by the agreement check. `STANLI_NO_TUNE`
gives a fixed graph for A/B work. The vectorization harness sets it for
the op-count cells, which compare structure, and leaves it unset for the
timing cells, which measure what ships.

`tune()` takes its measurer through an interface so a unit test can inject
recorded batch times and check the decision rule: flips when the
alternative is faster in every round; does not flip on a tie; does not
flip when the graphs disagree at a point even if the alternative is faster;
respects the budget; leaves a choice alone when no point evaluates. A
second test drives the carver's replay: replaying with no overrides
reproduces the same graph; overriding one candidate to leave reproduces
the graph the pass would have made without that island; the compile cache
is hit on replay.

## What the agreement gate excludes today

Measured with `stanli_check --point 0..2` with and without
`STANLI_NO_ISLAND`: s2_index_mi, s2_car, s2_mm, mother and dogs agree
bitwise at all three points; sw_skewnormal, hmm_gaussian and iohmm_reg
differ in the last bits of some gradient components at one or two points.
Their islands replace a vectorized kernel's reduction with a scalar loop,
and the adjoint of a broadcast scalar accumulates in a different order.
The gate therefore leaves those three as the estimate carved them, which
includes the one model that motivated this. Relaxing the gate to a ULP
threshold would tune them at the price of results that depend on timing;
that is a policy decision, not a code one, and the default stays bitwise.
Making those island forms accumulate in the kernel's order is separate
work that would also turn `test_hmm_parity` from a tolerance into an
equality.

## Budget check

sw_skewnormal preps its log_prob graph in 0.75 ms (island stage 0.12 ms)
and evaluates a gradient in 1.3 µs. Its one choice costs a replay carve
with cached compiles, two executor constructions over 16 ops, six
agreement evaluations and six to fourteen batches near the clock's
resolution floor, inside the 0.75 ms budget on the early-stop path. Models
whose gradient takes longer than their compilation (the ODE models) get no
rounds and keep the estimate's choice.

## Scope

Islands are the first client. Partition's cost decision, reroll's density
disposition and the write_array graph can register choices the same way;
none does in this change. The estimate stays as the ranking that decides
what is worth compiling; the join and split bounds stay as the pre-filter
that decides what is worth measuring.

## As built

The carver snapshots its op list at the start of a carve instead of
keeping the discarded vector, because `emit` renames later ops in place;
the pristine pre-carve graph is assembled from that snapshot and the
graph's slot and pool prefixes only when a decision inside the trust
radius exists. The reduce stage runs after the carver, so a replay
re-reduces. Three rules were added after measuring: a decision is a
choice only when the estimate priced both sides (a guard-skipped span is
never compiled to be measured); choices are tried closest call first and
only inside a trust radius of 0.25 on the estimate's own ratio; the
budget is one thousand gradient evaluations rather than a fraction of
compile time, and a batch is at least 20 µs long. With those, tuning
costs nothing on models whose decisions are far from the boundary and
0.2 to 0.4 ms on the sub-2 ms models whose one decision is close.

## Removed

Removed: `tune.cpp`, `tune.hpp`, `carve_plan.hpp`, the plan, cache and
override machinery in `island.cpp`, and the tests that existed only to
serve them. It was off by default and stayed off: the agreement gate
samples three points, not a per-instruction contract between an island
and the graph kernel it replaces, and nothing was building toward closing
that gap. What would make tuning worth having again is not more machinery
on this side but a corpus-level fix to the estimate's own bias, so the
boundary it draws needs a measurement to resolve less often. The last
revision that carried this pass is `13cd4ee9`.
