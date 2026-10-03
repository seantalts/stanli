# OP_REGION_MAP: one compiled body, run once per observation

Status: spec for implementation, revised after Codex review (2026-10-02).
Replaces Phase 1 of [the replay plan](2026-10-02-loop-replay-regrouping-plan.md).

## Why this instead of repairing frames

Under sampling the structured loop re-records whenever an observation changes
branch. Per-observation runtime-control regions never re-record, and the
region program itself costs 1.3 to 1.8x a replay per observation at a fixed
point (fwd+bwd per observation: gamma 172 vs 94 ns, exgaussian 258 vs 204,
geg 540 vs 386). Their remaining costs are around the program, not in it: one
graph op, scalar INDEX ops and a compile per observation (invgaussian: 115 s of
preparation). Compiling the body once and running it per observation keeps the
program and drops the rest. No recording, no guards, no frame replacement.

`OP_REGION_MAP` is a new graph op chosen at lowering. It does not change
`OP_LOOP`; loops that do not qualify keep their current path.

Every cogmod model has the same loop:

    for (n in 1:N)
      target += cogmod_X_lpdf(Y[n] | a[n], b[n], ..., s);

with whole vectors indexed by `n`, a few scalars, and parameter-dependent
branches inside the UDF. The first version targets exactly that shape and
refuses everything else.

## Step 0: native adjoint for DYN_INDEX

`DYN_INDEX` (`d = reg[a + c + idx - 1]`, `idx = val[b]`) is marked
`kProgramNoAdjoint`, so `gen_adjoint` refuses any program that indexes by a
runtime register. The map needs it: `Y[n]`, `a[n]` all become `DYN_INDEX` over
whole live-in vectors.

- Backward rule: `adj[a + c + idx - 1] += adj[d]`, with `idx` read from the
  forward value of register `b` (which must survive to the backward, or be
  checkpointed by the existing survival machinery).
- The indexed run `[a + c, a + c + len)` must keep identity adjoint cells
  (`no_alias`), like a ranged density's argument run.
- The index register gets no adjoint.
- Drop `kProgramNoAdjoint` from `DYN_INDEX`. Check every other reader of that
  flag (`program.cpp` fill sinking, `used_program_inputs`, `lower_stmt.cpp`'s
  unmodelled-range scan, the hard-coded exception lists) keeps its meaning.
- Tests: a runtime-control region that indexes a parameter vector by a data
  integer (and by a runtime loop variable inside the region) gets a native
  adjoint, and its gradient matches Stan Math autodiff. Out-of-range indices
  still throw the same exception.

This lands as its own commit before the map.

## Qualifying loop

All of:

1. `for` loop in log_prob lowering (`Lowering::lower_stmt_impl`, `For` case,
   `runtime/src/lower_stmt.cpp`) with bounds folded at lowering and trip count
   at least 32. Not write_array, not GQ, not inside another region.
2. **Parameter-dependent control.** Reuse `region_auto_profitable(s, {lo, hi})`
   (`runtime/src/lower_structured_loop.inc`), the test that selects the
   structured loop today: trip count at least 32, outermost structured depth,
   and `region_runtime_control`, which finds an `if`, ternary, `&&`/`||` or
   `while` whose condition is not data-only, following UDF calls. Not
   `needs_runtime_control`, which also fires on data-only control. Refuse when
   `target_scale != 1.0`, as the structured loop does.
3. **Independent iterations**, decided by binding, not by name. Every name the
   body assigns resolves to a declaration inside the body; the loop variable is
   never assigned; the body reads no local that an earlier iteration wrote. Any
   name declared before the loop and assigned in the body refuses the loop
   (this covers O1-inlined return symbols hoisted above the loop). No `break`,
   `continue` or `return` that leaves the body. The body does not read
   `target()`. Integer locals in the body follow the typed integer contract
   the region compiler already uses.
4. The only effect on the outside is `target +=`. No effectful statements
   (`stmt_effectful`), no RNG, no print/reject other than what the region
   compiler already supports with the same exception behavior.
5. Every `CALL` in the compiled program is replayable: running its forward
   twice with the same inputs gives the same result and the same scratch.
6. The region compiler accepts the body with the loop variable bound to a
   runtime register, and `gen_adjoint` succeeds. Otherwise fall back to
   today's path with no state changed (see "Transactional lowering").

## Compilation (`Lowering::lower_region_map`, modelled on `lower_island`)

- Build a `ProgramCompiler` as `lower_island` does for a statement region over
  the loop body, with the same extern hooks and seeded constants.
- **Iteration register.** Allocate one register for the loop variable and bind
  the name to it as the runtime-`for` path does
  (`reals[s.loopvar] = Range{index, 1}`; `ints` must not contain it). The
  register is written by the kernel before each run, so initialization analysis
  must treat it as written at entry and `compact_island` must remap it. The
  simplest way is to make it a live-in of length 1 whose value the kernel
  overwrites; an explicit seeded-register field is fine too.
- **Live-ins.** Bound as `lower_island` binds them. Whole vectors are expected;
  they are copied into the register file once per evaluation. More than six
  live-ins use the existing packed input. The body's instruction count and its
  non-live-in register count must not depend on N. Check that whole-vector
  live-ins do not hit the initialization analysis storage cap; raise or bypass
  it for live-in ranges if they do.
- **Target.** `target +=` accumulates into the program's target register,
  seeded to zero at the start of each run, so each run yields one iteration's
  term, the same per-iteration subtotal today's per-observation regions
  produce.
- **Output.** One scalar: the sum of the per-iteration terms in iteration
  order. Lowering pushes it as one target term. This reassociates the sum
  relative to a per-term prefix fold; the ULP policy accepts that, and the
  tests below bound it.
- **Transactional lowering.** `lower_island` can mutate `int_env` (it writes
  folded integers back), create slots (`uninitialized_decl_slot`,
  `current_target_slot`) and then throw. Run every eligibility check and the
  whole compile, including `gen_adjoint`, before emitting any graph op, and
  restore what the attempt touched on refusal, so a refused loop lowers
  exactly as it does today. `fork_region_trial()` is the audited helper the
  structured loop uses for the same problem; use it or match what it saves.

## Kernel (`OP_REGION_MAP`, `runtime/kernels/`, registered like `OP_ISLAND`)

- idata: `lo`, trip count. udata: the program, its generated adjoint, the
  iteration register, the adjoint clear spans.
- Forward: copy live-ins into the register file once; for each iteration, set
  the iteration register, run the program, add its target register into the
  output.
- Backward recomputes: for `i` from last to first, rerun the forward program
  for iteration `i`, then run the generated adjoint seeded with `out_adj`.
  - The forward's live-in values must be the ones the forward saw. Either
    re-copy from the input slots (if the executor guarantees they are
    unchanged between forward and backward) or keep a snapshot in scratch.
    Refuse any program that writes a live-in register.
  - Adjoint storage keeps the generated numbering. Mark the persistent cells
    (live-in cells, through `adj_reg`). Precompute the transient spans: every
    adjoint cell that is not persistent. Zero the whole adjoint file once per
    backward; between iterations clear only the transient spans. Clearing the
    whole file per iteration is O(N) per iteration, the bug fixed in #423.
  - Verify at compile time that no transient cell, and no destination the
    adjoint consumes or clears, overlaps a persistent cell. Refuse otherwise.
  - After the last iteration, write live-in adjoints back to the graph exactly
    as `OP_ISLAND` does, including its continuation policy for inputs whose
    adjoints flow elsewhere.
- Keep the island's index validation and exception behavior: an out-of-range
  index or a domain error throws the same exception type and message at the
  same iteration.
- A copied executor (multi-chain) gets its own register file and scratch.

Selection: in the `For` case of `Lowering::lower_stmt_impl`, after the bounds
fold and before `try_lower_region`, in both the main lowering and the bounded
specialization trial. On by default; `STANLI_REGION_MAP=0` turns it off for
ablation. Skipped when `STANLI_STRUCTURED_LOOPS` forces or prefers the
structured loop, so tests that ask for `OP_LOOP` keep getting it. With
`STANLI_STRUCTURED_LOOPS=0` the map still applies. Everything else is
unchanged.

## Tests (RED first)

1. **Same executor, two parameter points.** Compile once, bind one executor,
   evaluate log density and gradient at two parameter points where several
   observations change branch between them (a tail switch
   `if (y[n] - mu < c)` with `mu` moved across several observations). Check
   both points against Stan Math autodiff computed in the test. Assert the
   graph has one `OP_REGION_MAP` and no `OP_LOOP` or per-iteration
   `OP_ISLAND`. Do not benchmark at a fixed point; a fixed point never flips a
   branch.
2. **Size does not grow with N.** The body's instruction count and its
   non-live-in register count are the same at N = 64 and N = 512.
3. **Refusals keep their path:** a loop carrying `s = s + theta` across
   iterations; a name declared before the loop and assigned inside; a body
   with `break`; a body reading `target()`; data-only control (keeps today's
   lowering); trip count below 32.
4. **Shapes:** more than six live-ins (packed); the same vector passed as two
   arguments (shared input alias); a live-in whose adjoint also flows to other
   ops (external adjoint); a branch-local integer; a CALL (density or packed
   kernel) whose scratch is reused across iterations; several `target +=` per
   iteration with other terms before and after the loop; an out-of-range index
   that must throw.
5. **Copied executor:** two executors from one compiled model, evaluated
   interleaved, agree with a single one.
6. Direct CmdStan checks on two cogmod models (exgaussian, lnr_bench): log
   density and gradient within the ULP policy at two parameter points.
7. Full CTest and the corpus replay (`tools/verify_refs.py`), all 351 models.

## Measurement

Sampling only: `stanli_run`, seed 1, warmup 50, samples 50, one chain,
per-gradient cost = wall / total leapfrogs, identical leapfrog counts versus
the comparison run. All cogmod families, against the loop and regions columns
in the replay plan. Expect gamma, weibull and logstudent to land between their
loop and regions numbers until batching (Phase 2) recovers the dispatch cost;
exgaussian, geg, lnr_bench and the timeouts should drop to the regions numbers
or below. Preparation time for invgaussian and lnr_bench via
`bench_grad --prep`.

## Out of scope for this step

Storing per-iteration register files instead of recomputing; batched (lane)
execution of the map program (Phase 2); GQ/write_array; while loops; nested
maps.
