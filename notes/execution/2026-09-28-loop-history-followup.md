# Continued native execution experiments

Historical experiment: its implementation and build targets were removed before
merge. The commands below describe the recorded experiment at commit
`ac4be527`, not tools in the current tree. Results and limitations remain
part of the research record. See the [cleanup audit](2026-09-29-execution-code-cleanup.md).

Authorization: continue experiments, keep improvements, avoid regressions, and
consolidate existing execution machinery. No new production engine or dispatch
JIT. Base `5b878354`, synchronized to `origin/HEAD` at `6ce2018b`.

Acceptance: Stan semantics and bitwise local/coupled-solver comparisons first;
then native preparation, first/warm gradients, complete solves, and history
storage at small/medium/stress sizes. No candidate is enabled merely because
its warm large case improves. Retain existing faster paths. Cross-model tests
can establish measured coverage, not a universal no-regression guarantee.

Experiments queued:

1. **Selective history.** Use the existing opcode primal-read metadata to keep
   only values read by reverse. Execute control in reusable forward storage;
   constants and integer/control results need no differentiated identity.
   Prediction: lower history bytes and forward recording cost. Refuse operations
   outside the existing scalar proof. Compare against prior full-frame history.
2. **Guarded prepared-path reuse.** Counterproposal to optimizing every recording:
   prepare an ordinary SSA register/adjoint program for an observed path, with
   branch guards checked on every call. Reuse code, never parameter values or
   branch decisions. On a changed path, fail the guard before executing its
   successor, then re-record through the correct path. Scalar pure operations
   permit this prefix replay; effects and kernel calls remain excluded. Bound
   cached paths and prepared sizes. Prediction: warm execution approaches the
   existing expanded-register path, at a preparation/storage cost that must be
   measured and can disqualify production use.
3. **Coverage and simplification.** Revisit remaining interpreter refusals with
   existing register lowering. Prefer eliminating a redundant refusal or
   representation over introducing another evaluator. Carry successful changes
   through independent CmdStan checks and native model-level comparisons.

Decision gates: investigate at most one further concrete storage optimization
if selective history remains slower; test the materially different reuse
hypothesis before more local tuning. Keep positive mechanisms only where total
costs and correctness support them. Record failed variants with their scope.

## Results and decisions

The production improvement is [nested-loop coverage](2026-09-28-nested-loop-coverage.md).
Eligible callbacks now use the existing register engine, with the same Stan Math
derivatives. This is the change to keep enabled. The three production engines
remain unchanged in number; another interpreter fallback is removed.

Selective history uses the existing opcode metadata (`kProgramSaveA/B/C/Out`)
to identify primal values needed by reverse. Constants and integer/control
results share a discarded adjoint cell only when no derivative rule consumes
that cell. It reduces retained values, but recording and rebinding instructions
on every invocation still loses. Keep it as an isolated comparison and as the
correct miss path for the following developer experiment, not a production route.

Guarded reuse compiles an observed path into an ordinary register program and
its existing generated adjoint. Copies retain shared value identities. Every
conditional decision has a guard before its successor. A failed guard stops
that attempt; the pure scalar prefix can then be evaluated again by the recording
path. Values and derivatives are recomputed on every call. Nothing caches a
parameter value or assumes that a previous branch is still correct.

The prototype refuses effects, kernel calls, and unsupported scalar operations.
It holds at most four paths and refuses expansion beyond a conservative
100,000-register estimate. A changed path, cache eviction, repeated reverse,
workspace copying, and failure recovery are covered by the developer checks.
The bounds are prototype resource limits, not a production admission policy.

### Complete native solve comparison

Six fresh processes per callback/size/history; 108 total. Each process requires
bitwise local values/Jacobians, bitwise complete solution values/Jacobians, and
equal solver callback counts before timing. Two counterbalanced paired batches
follow 50 ms warmup per arm, with 30 solves at 8/128 iterations and three at
2,048. Builds and tests did not overlap timings.

| Callback | Iterations | Current paired path µs | Cached path µs |
| --- | ---: | ---: | ---: |
| Simple retained loop | 8 | 3.10 | 1.78 |
| Simple retained loop | 128 | 31.17 | 16.76 |
| Simple retained loop | 2,048 | 439.24 | 239.74 |
| Branching retained loop | 8 | 46.40 | 22.64 |
| Branching retained loop | 128 | 286.68 | 133.51 |
| Branching retained loop | 2,048 | 5,089.26 | 2,253.99 |

Guarded reuse is a repeatable warm improvement, roughly 1.7–2.3× in these
complete solves. Expanded and selective per-call histories remain slower than
their paired current path. All samples and median absolute deviations are in
[the experiment artifact](data/2026-09-28-loop-history-performance.json).

These warm gains do **not** justify default routing. In exploratory local phase
runs, the simple 2,048-iteration callback takes about 0.77 ms for its first
prepared-path gradient, versus 36 µs warm; the branch callback takes 1.71 ms
first versus 69 µs warm. Their current warm callback controls are about 61 and
145 µs. Those are first uses of a prepared probe in an already running process,
not matched whole-model cold-start measurements.

After the local branch/copy matrix, accounted live history and cached-plan
storage reaches roughly 2.08 MB for the simple case and 6.11 MB for the branch
case at 2,048 iterations. It includes the current primal/adjoint workspace and
cached instruction/pool/binding elements. It excludes reserved capacity, vector
objects, allocator overhead, temporary construction peaks, and other model
state. Repeated new paths can also repeatedly pay compilation cost. None of
these costs belongs on a default path without a stronger admission argument.

### Validation and reproduction

Across three histories and nine local configurations each, 2,592 weighted local
comparisons and 2,268 alias-history comparisons pass bit for bit against the
existing register/Stan Math replay. These exercise zero/one/many iterations,
aliasing outputs, overwritten values, more than four changing paths, repeated
reverse, copied workspaces, invalidation after a failed forward, and refusal of
unsupported operations. The recorded CmdStan evidence remains separate: this
probe does not resolve the earlier shared 24-ULP branch stress discrepancy.

Build `bench_loop_adjoint` and `bench_loop_adjoint_local`. Run the paired solve
experiment with:

```
python3 tools/bench_loop_adjoint.py --histories expanded selective cached \
  --output /tmp/loop-history-results
```

For local comparisons use `STANLI_LOOP_HISTORY=expanded|selective|cached` with
`bench_loop_adjoint_local MIR N`. These environment choices are developer-tool
options; the production runtime has no new execution selector. The prior mapped
history patch belongs to the historical `5b878354` checkout.

Keep the cached prototype for future work on preparation and admission, but do
not enable it in production. Prefer further shared-compiler coverage fixes with
whole-model wins over adding another evaluator or forcing engine consolidation
that would make currently fast models slower. Dynamic storage, general runtime
call frames, and the remaining initialization/interpreter roles are still open.
