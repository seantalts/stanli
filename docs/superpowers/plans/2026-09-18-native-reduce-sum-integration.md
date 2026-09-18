# Native reduce_sum integration

Initial integration base: fetched origin/HEAD = origin/main at
`504e8d80ca05ef1961084ac904c557c698552823`; verified ancestor before this
stage. Existing prototypes and their raw evidence are preserved. The unchanged
stage-two executable and libraries are saved in
`build-reduce-sum-main/native-baseline/` with an active-slice baseline run.

Authorized objective: native within-chain parallel reductions with reusable,
compact storage and measured throughput. Zero allocation during gradients is
not a requirement. Default compilation remains serial; native C++ and CLI
opt in. Prior design and compact-import reviews are linked in adjacent records.

## Latest-main integration

Main advanced during validation. Fetched again and fast-forwarded to
`5931b66623438685cc11c64d2968b7a6c96fb237` (Stan 2.40 / Stanli 0.15.0 preparation).
The task changes applied cleanly, and origin/HEAD ancestry was verified before
rebuilding. Recovery stash `808810ebe32765a2e72033d6fa8950fd55b07ba5` remains saved.
The pre-update results below are historical; clean native240 builds and current
validation are recorded at the end of this document.

Dependencies now match main: Math `5252d51d47c1d5e78005fc043ad996fad6dd8da8`,
Stan `a6806ef8477a7b5f65b27449ec33528c162bc024`, stanc3
`d58446e631b02cacc5355e373defc6092a684554`. Reused validated dependency artifacts
through task-local symlinks without changing another worktree. The cached
embedded frontend passes the current producer-stamp check. The only Math patch
is the repository's existing CVODES backward-quadrature initialization patch.

## Evaluator and decision gates

- Semantics: differential values/gradients against default serial lowering;
  fixed-partition repeatability, independent polynomial/density oracles,
  non-unit adjoints, active/data slices, aliasing, bounds, empty slices,
  argument effects, fallback, value-only calls, clones, exceptions/recovery.
  Reassociation deliberately changes rounding: differential tolerance is
  1e-10*(1+abs(reference)), with exact equality for repeat evaluations of a
  fixed partition and worker-count changes.
- Performance: matched Release/O3 native ordinary-model gradients against
  the existing whole-slice path, warmup and repeated counterbalanced samples;
  N=100000 normal/Student, active/data slices plus small-job cutoff. Provisional
  continuation gate: a clear large-workload speedup, not a promise for all
  callbacks. Preparation and retained memory are separate measurements.
- Generality: static shapes, pure callbacks, fixed-index transparent graphs;
  transactional refusal of runtime control, effects and opaque kernels.
  No model or identifier matching. Full relevant native tests, unrelated
  executor canaries, and TSAN exercise the shared executor changes.
- Delivery: reproducible native test/benchmark commands, opt-in C++/CLI,
  documented thread budget and fallback boundaries, reviewed implementation.

## Representation choice

Stage-two experiments selected compact child graphs with retained buffers over
full per-child parent copies. Native forward and reverse dispatch separately;
reverse receives the actual upstream scalar adjoint. This avoids invalid
unit-seed rescaling for reductions embedded in nonlinear expressions. Children
copy only proved input ranges and return compact parameter gradients; parent
adjoints accumulate in deterministic child order. Owner participates in a
persistent team. Gradient allocation may occur in kernels or exceptional paths.

Single shared arena partitioning remains a possible allocator refinement: it
would change ownership of backing allocations, not eliminate the need for
independent saved child values and overlapping shared-parameter adjoints.
Exclusive-gradient publication was beneficial in the isolated probe; the first
native integration measures canonical owner merging before adding that extra
concurrency contract. Scheduler overhead motivates retaining serial compilation
below a provisional outer-element cutoff. The initial 2048 cutoff was
raised to 8192 after the cutover experiment: N=2048 lost on both densities,
N=4096 lost on normal and was neutral on Student, N=8192 was neutral on
normal and 1.50x faster on Student. This is a heuristic, not a universal
callback cost model. See native-cutover.json in the raw evidence.

## Current state

Native C++/CLI integration is implemented and validated. Fable
[confirmed the three blockers are resolved](2026-09-18-native-reduce-sum-integration-review-followup.md).
Initial differential tests caught
constant folding reading uninitialized data imports in child graphs. Child graph
constant folding is now disabled: imported inactive inputs are runtime bindings,
not compile-time fills. The regression matrix includes both active and inactive
sliced inputs so this cannot hide behind data-only folding.

## Validation so far

- Clean native ABI build, Release/O3, `-ffp-contract=off`, STANLI_THREADS=ON.
  All 253 originally configured tests passed. The subsequently added CLI
  regression and reduction suite also pass after raising the cutoff.
- ThreadSanitizer: reduction lifecycle/semantic suite plus 49 native benchmark
  configurations passed. Final cutoff/header rebuild is being repeated.
- Tests cover analytic nonlinear polynomial and density oracles, >6 operands,
  aliases, active slices, nested and static calls, propto, exact refusal graph
  and fill equality, actual non-unit reverse seeds, invalid reverse states,
  lowest-chunk error selection and draining, recovery, reentry rejection,
  concurrent cloned executors, and repeatable multi-chain NUTS.
- Initial source-suite failures were toolchain setup failures, not waived
  assertions: no frontend was configured, then the shared cached embedded
  object had an old producer hash. A copied cached object with the exact
  current producer hash `9d944c075cf4f3d7ed069ab6e6a431f7246d9963afefa173082c4f4323e44ea7`
  and pinned stanc3 source `8e154ac3454790cb5427ae72106c859b3fb8d90a` passed
  `stanc_embed_artifact_matches`, and the full suite passed with it. Shared
  dependency artifacts were left untouched. fmt was fetched at the pinned
  `40626af88bd7df9a5fb80be7b25ac85b122d6c21` revision.

## Quiet native performance

Ten counterbalanced samples per workload, 20 warmup gradients per mode, at
least 20 timed iterations per sample (2,000,000/N, rounded down). The modes
share the binary, MIR, dataset and parameter point. Whole is the default
whole-slice compiler; native serial runs the retained partition on the caller;
native parallel uses the persistent team. Raw native-performance-final evidence
records timings, commands, hashes and build/dependency identities.

At N=100000, four workers: Student data slice 948.52 -> 317.53 us (2.99x),
Student active slice 983.81 -> 385.07 us (2.55x). The slower retained single-worker
ablation is expected and is not the default one-thread compiler path. Twenty
warm parallel evaluations observed zero ordinary C++ new/new[] calls for these
fixtures; aligned allocations/malloc and other callbacks are not covered by
that diagnostic. Peak RSS includes compiler/probe/oracle states and is not an
incremental native-runtime memory claim.

An unrelated arithmetic executor canary was compiled twice against the saved
base headers/library and the candidate, running the same two-op square/sum
program. Ten counterbalanced process pairs gave medians base -> candidate:
N=1: 14.44 -> 15.50 ns; N=64: 102.91 -> 109.25 ns; N=4096: 6326.78 -> 6201.19 ns.
IQRs overlap at all sizes. The tiny fixed overhead is visible in medians;
these measurements do not prove zero serial overhead. The new readiness/seed
contract adds per-evaluation work, not a per-op dispatch branch. Large reduction
speedups remain clear; no claim is made that every callback benefits.


## Fable review fixes and final validation

The [implementation review](2026-09-18-native-reduce-sum-integration-review.md)
found no kernel/scheduler/planner wrong-answer issue, and identified three
integration blockers. The fixes are:

1. Copy CompileOptions into bounded specialization trials. A dedicated fixture
   requires a retained reduction, checks its polynomial oracle, and preparation
   tracing confirms `graph=bounded_log_prob` actually ran.
2. Admit only immutable BoundCheckSpec payloads (lower/upper/matching-dim checks)
   in the import planner. Their full input reads and validation remain intact.
   The nested fixture now requires two retained outer operations; nested
   reductions execute serially inside their child.
3. Remove the broad logic_error catch. Expected CompileError refusals and
   structural predicates populate unique `reduce_sum_fallbacks`; the CLI prints
   them. Internal logic errors propagate. Tests pin visible budget/dynamic
   refusal and identical fallback graph/fills.

Further coverage includes integer-array Poisson reductions with normalized and
proportional spellings, two retained calls in one graph, and an effectful shared
argument evaluated once outside an accepted pure callback. Static partition,
packed >6 operands, failure/recovery, TSAN, and refusal equivalence were already
added while the first review ran.

After these fixes: **254/254 CTest tests pass**, the TSAN reduction suite passes,
and **49/49 native differential configurations pass in both Release and TSAN**.
The current raw artifact is [native-reduce-sum-integration.json](../../native-reduce-sum-integration.json),
including commands, hashes, phase timings, memory diagnostics, cutoff experiment,
canary samples, full-suite result and TSAN evidence. Final performance rerun uses
the reviewed implementation; numbers below supersede the preliminary table above.

Reproduce (run from repository root):

```sh
cmake --build build-reduce-sum-native --target test_reduce_sum stanli_run bench_reduce_sum -j 8
ctest --test-dir build-reduce-sum-native --output-on-failure -j 8
python3 tools/bench_reduce_sum.py --native --binary build-reduce-sum-native/bench_reduce_sum --output native-evidence
build-reduce-sum-native-tsan/test_reduce_sum
python3 tools/bench_reduce_sum.py --native --binary build-reduce-sum-native-tsan/bench_reduce_sum --checks-only --output native-tsan-evidence
```

The native tree is configured with the verified embedded frontend and OCaml 5.5
runtime; the TSAN tree uses `-DSTANLI_SANITIZE=thread`. Both use Release/O3,
`STANLI_THREADS=ON`, and `-ffp-contract=off`. No commits, PR, or merge have been
created. Work remains in the existing worktree based on the fetched main SHA
reported above; preserved recovery stash and prior prototype artifacts remain.

| N | P | Kind | Active slice | Threads | Whole µs | Chunks serial µs | Parallel µs (IQR) | Speedup vs whole | Peak MiB |
|--:|--:|:--|:--|--:|--:|--:|--:|--:|--:|
| 1000 | 3 | normal | False | 4 | 5.36 | 5.38 | 5.40 (0.59) | 0.99x | 8.4 |
| 1000 | 3 | student_t | False | 4 | 9.67 | 9.75 | 9.75 (0.95) | 0.99x | 8.4 |
| 10000 | 3 | normal | False | 4 | 52.81 | 65.03 | 44.98 (2.01) | 1.17x | 12.3 |
| 10000 | 3 | student_t | False | 4 | 94.60 | 102.96 | 53.61 (4.57) | 1.76x | 12.5 |
| 100000 | 3 | normal | False | 4 | 528.57 | 640.14 | 234.31 (22.97) | 2.26x | 55.2 |
| 100000 | 3 | student_t | False | 4 | 930.13 | 1024.02 | 314.92 (11.08) | 2.95x | 55.4 |
| 100000 | 3 | student_t | False | 1 | 938.82 | 1043.22 | 1043.21 (93.97) | 0.90x | 54.9 |
| 100000 | 3 | student_t | False | 2 | 895.46 | 998.91 | 510.37 (34.85) | 1.75x | 54.9 |
| 1000 | 3 | student_t | True | 4 | 10.13 | 10.23 | 9.34 (1.03) | 1.08x | 8.4 |
| 10000 | 3 | student_t | True | 4 | 88.59 | 102.17 | 61.10 (5.39) | 1.45x | 13.4 |
| 100000 | 3 | student_t | True | 4 | 976.47 | 1115.15 | 382.66 (10.99) | 2.55x | 65.8 |
| 10000 | 256 | student_t | False | 4 | 92.48 | 106.61 | 51.96 (6.01) | 1.78x | 12.6 |
| 10000 | 4096 | student_t | False | 4 | 95.99 | 123.07 | 66.09 (7.97) | 1.45x | 13.6 |


Final review: Fable found no remaining blocker. One optional diagnostic nit
remains: multiple empty-slice sites can emit repeated empty-slice notes. It does
not affect execution. The zero-seed reverse regression also passes in Release
and TSAN. Native within-chain execution stays opt-in; runtime programs and
interpreter contexts retain serial execution, and refusal notes describe graph
lowering decisions rather than an exhaustive inventory of every nested call.


## Final results on latest main / Stan 2.40

**Current validated base: `5931b66623438685cc11c64d2968b7a6c96fb237`.**
The compiler/runtime changes reapplied without conflicts. Clean builds use the
new pins above; the Fable-reviewed reduction algorithm is unchanged.

- 259/259 configured CTest tests pass, including the CLI regression and Stan
  2.40 additions.
- Native TSAN test_reduce_sum passes (including zero seeds, worker errors,
  recovery, concurrent clones and NUTS); 49/49 differential configurations pass
  under both Release and TSAN.
- Four-thread Student-t at N=100000: data-only slice 928.40 -> 313.54 us (2.96x);
  active slice 970.22 -> 384.82 us (2.52x). Normal data slice: 536.13 -> 230.48 us
  (2.33x). Fixed-partition single-worker execution remains an ablation, and
  default one-thread compilation continues to use whole-slice lowering.
- The latest raw JSON has these measurements at its top level. Prior-base
  data, including the arithmetic canary and cutoff experiment, is explicitly
  retained under `prior_base_504e8d80`; it is not relabeled as a Stan 2.40 result.

Current commands (same flags/compiler configuration as above, clean build
names distinguish the dependency ABI):

```sh
cmake --build build-reduce-sum-native240 -j 8
cmake --build build-reduce-sum-native240 --target bench_reduce_sum -j 8
ctest --test-dir build-reduce-sum-native240 --output-on-failure -j 8
python3 tools/bench_reduce_sum.py --native --binary build-reduce-sum-native240/bench_reduce_sum --output native240-evidence
build-reduce-sum-native240-tsan/test_reduce_sum
python3 tools/bench_reduce_sum.py --native --binary build-reduce-sum-native240-tsan/bench_reduce_sum --checks-only --output native240-tsan-evidence
```

| N | P | Kind | Active slice | Threads | Whole µs | Chunks serial µs | Parallel µs (IQR) | Speedup vs whole | Peak MiB |
|--:|--:|:--|:--|--:|--:|--:|--:|--:|--:|
| 1000 | 3 | normal | False | 4 | 5.47 | 5.76 | 5.40 (0.58) | 1.01x | 8.4 |
| 1000 | 3 | student_t | False | 4 | 9.70 | 9.65 | 9.71 (0.41) | 1.00x | 8.4 |
| 10000 | 3 | normal | False | 4 | 52.97 | 67.00 | 45.36 (1.21) | 1.17x | 12.5 |
| 10000 | 3 | student_t | False | 4 | 94.63 | 105.66 | 54.28 (4.97) | 1.74x | 12.7 |
| 100000 | 3 | normal | False | 4 | 536.13 | 659.79 | 230.48 (17.63) | 2.33x | 55.3 |
| 100000 | 3 | student_t | False | 4 | 928.40 | 1043.94 | 313.54 (10.85) | 2.96x | 56.5 |
| 100000 | 3 | student_t | False | 1 | 940.79 | 1012.74 | 1011.18 (45.05) | 0.93x | 55.0 |
| 100000 | 3 | student_t | False | 2 | 921.96 | 1043.42 | 539.85 (10.56) | 1.71x | 55.0 |
| 1000 | 3 | student_t | True | 4 | 10.33 | 10.11 | 9.90 (0.46) | 1.04x | 8.4 |
| 10000 | 3 | student_t | True | 4 | 96.52 | 111.20 | 61.12 (7.84) | 1.58x | 13.5 |
| 100000 | 3 | student_t | True | 4 | 970.22 | 1098.33 | 384.82 (23.95) | 2.52x | 65.9 |
| 10000 | 256 | student_t | False | 4 | 91.20 | 104.81 | 56.32 (4.24) | 1.62x | 12.6 |
| 10000 | 4096 | student_t | False | 4 | 98.81 | 129.13 | 62.45 (8.54) | 1.58x | 13.6 |
