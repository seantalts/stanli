# Native reduce_sum: compact imports and gradient publication

Current authorization: continue the reviewed within-chain parallelism design,
prioritizing performance and reasonable memory over a strict allocation ban.
The baseline is the stage-one developer prototype at repository HEAD
`be0a0c8d4882afca3e1d1de0083267df10f8fb3d`; its source, runner, executable and a
fresh pre-edit active-slice measurement are retained in the ignored
`build-reduce-sum/stage2-baseline/` directory.

Current checkout: fetched and moved to `origin/main` at
`504e8d80ca05ef1961084ac904c557c698552823` on user request. The previous checkout
was 136 commits behind. Prototype files were preserved byte-for-byte; the only
merge conflict combined both developer benchmark targets in CMake. Recovery
stash: `ac9820460c3e2f72b688c4c3c97a45c102ab797e`. The old builds and measurements
remain historical evidence. A fresh Release build, the 49-case matrix, and
executor/multichain/reduce_sum regression tests pass on the new base.
Current main already separates immutable data from mutable values and shares
that data across Executor clones; the storage design must reuse this facility.
The unfinished follow-up Fable review was cancelled and restarted on this base.

Status on the old base: the isolated import planner and worker publication were
implemented and verified. The historical evidence supports carrying both into
retained-reduction lowering, subject to new-base validation.
Production lowering, public thread controls, seeded reverse, and a shared arena
binding interface remain unimplemented. No runtime source or ABI changed.

[Stage-one record and stage-two evaluator](2026-09-18-native-reduce-sum-prototype.md)
| [Raw stage-two evidence](../../native-reduce-sum-imports.json)

## Implementation and proof

`tools/reduce_sum_imports.hpp` plans one contiguous import per source slot. For
an unwritten input, static `OP_INDEX` / `OP_SLICE` consumers contribute their
read ranges; the import retains their convex hull. Multiple or overlapping
reads of the same source share that import. A whole-input or unknown consumer
keeps the entire input. Unreferenced leaves lose their storage. Written slots
and the result retain their full size. Integer offsets and data fills are
rebased into the retained ranges.

The planner preserves every operation, operand order, output size, density
activity bit, and reverse accumulation order. It changes storage and input
addresses only. In particular, it does not erase effectful operations or assume
that an unused result licenses removing its producer. Opaque payloads, mutable
kernel state, dynamic geometry / dynamic indexing, and in-place updates refuse
before publication. Malformed fixed ranges and parameter-offset overflow also
refuse. Graph, fills, and import maps commit together after preparation.

The map names original parent parameter coordinates and compact child
coordinates. Source slot identity supplies the same alias-coalescing rule used
by retained-loop lowering. This probe starts from ordinary compiled graphs; it
does not expose or modify that lowering's private interfaces or CompiledModel's
public parameter metadata.

At preparation, a scatter plan counts readers per parent coordinate. A range
read by one child can be published by that child after its gradient completes.
Overlaps split the publication map and retain canonical chunk-order merging on
the owner. The owner clears the destination before dispatch. All jobs drain
before exceptions escape; a subsequent evaluation clears partial results. The
same persistent team runs full and compact children. Preparation allocates the
maps and arenas; ordinary C++ allocation diagnostics observe zero allocations
in the warmed successful evaluations tested.

This is direct publication of completed local gradients into exclusive parent
ranges. It still copies those gradients from each Executor's adjoint arena into
a local result, and copies compact parameter inputs before dispatch. It is not
zero-copy binding or one physical arena partitioned between children.

## Current-base measurements (504e8d80)

A fresh `build-reduce-sum-main` Release build uses the same pinned Math, Stan and
stanc revisions; the Math patch is now explicitly part of current main's
`deps/fetch.sh`. No shared runtime sources were modified by the prototype.
The fixture and all proof checks compile unchanged on the new base. The same
five-mode, ten-sample evaluator gives these **current-base** medians:

| N | Inputs / callback | Whole us | Full parallel us | Compact parallel us (IQR) | Full / compact | Child arenas MiB, full → compact |
|--:|:--|--:|--:|--:|--:|--:|
| 1,000 | data / Student-t | 9.77 | 12.54 | 12.15 (0.54) | 1.03x | 0.153 → 0.077 |
| 10,000 | data / Student-t | 92.26 | 36.11 | 36.66 (3.35) | 0.99x | 1.527 → 0.764 |
| 100,000 | data / Student-t | 857.63 | 244.66 | 246.10 (3.11) | 0.99x | 15.259 → 7.630 |
| 1,000 | active / Student-t | 10.12 | 19.14 | 16.85 (0.26) | 1.14x | 0.176 → 0.100 |
| 10,000 | active / Student-t | 89.85 | 56.20 | 45.17 (2.72) | 1.24x | 1.755 → 0.993 |
| 100,000 | active / Student-t | 986.53 | 447.44 | 308.49 (4.60) | 1.45x | 17.548 → 9.919 |
| 10,000 | data / Student-t, P=4096 | 95.51 | 49.45 | 48.38 (2.21) | 1.02x | 1.776 → 1.013 |

Four chunks and workers are used in this table. These are the final,
post-review-guard timings. The large active case is 3.20x faster than whole-slice
execution in this series, and 1.45x faster than the same full-child partition.
The pre-review current-base series is also retained: its corresponding compact
parallel median was 309.07 us versus 308.49 us here. The 1- and 2-thread rows are
noisy (up to 92 us IQR); they do not establish a tuned thread-count policy.
Memory counts include mutable values and immutable data even though current
Executor stores them separately. Children are independently compiled, so the
shared-data clone facility does not deduplicate data across these different
child graphs. No source-level name is used by import planning.

The final active-case phase diagnostics measure packing/clear at
46.42 → 14.71 us and owner merging at
73.98 → 0.125 us. Successful warm compact runs
observe zero ordinary new/new[] calls. The old-base publication ablation and
opcode profile below remain historical rather than being relabeled as current.

All 49 Release configurations, arithmetic/alias/empty/overflow refusal cases,
partially overlapping publications on the actual worker team, clone tests,
and exception/recovery checks pass on this base. `test_executor`,
`test_multichain`, and `test_reduce_sum` also pass. A fresh ThreadSanitizer build also passes all 49 configurations and a
15,000-evaluation stress run, with empty sanitizer stderr.
[Fable's current-base review](2026-09-18-native-reduce-sum-import-review.md) found
no blockers. Its low-severity parameter-layout guard finding was addressed;
post-review validation is recorded with the final source identity.

To configure this base from scratch:

```sh
cmake -S . -B build-reduce-sum-main -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTANLI_THREADS=ON
cmake -S . -B build-reduce-sum-main-tsan -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTANLI_THREADS=ON -DSTANLI_SANITIZE=thread
```

Then use `build-reduce-sum-main` and `build-reduce-sum-main-tsan` instead of the
historical build directories in the commands below. Raw current-base records are under `current_base` in the linked
JSON evidence; each series carries its own source/binary identities.

## Historical matched measurements (be0a0c8d)

Apple M3 Ultra, 32 logical CPUs, macOS 26.6.2, Apple clang 21, Release
`-O3 -DNDEBUG`, `-ffp-contract=off`, `STANLI_THREADS=ON`. Each process retains the
whole executor, original children, and compact children. Five execution modes
are counterbalanced across ten samples; all timing includes copies, clearing,
execution, dispatch, and publication/merge. Preparation and profile diagnostics
are separate. No build or other benchmark from this task overlapped timing.
The existing pinned Math checkout's one-line CVODES patch is recorded in the
metadata and is irrelevant to these non-ODE callbacks; it was not modified.

Four chunks/workers and three shared parameters unless noted. Times are median
microseconds; parentheses give compact parallel IQR. Arena sizes are the sum of
child values, adjoints, and scratch, excluding graph metadata and returned
local-gradient vectors. Process peak RSS includes both alternatives and is not
an estimate of a production chain's footprint.

| N | Inputs / callback | Whole | Full parallel | Compact parallel (IQR) | Full / compact | Child arenas MiB, full → compact |
|--:|:--|--:|--:|--:|--:|--:|
| 1,000 | data / Student-t | 9.77 | 12.35 | 11.93 (0.52) | 1.04x | 0.153 → 0.077 |
| 10,000 | data / Student-t | 90.88 | 36.56 | 36.21 (1.49) | 1.01x | 1.527 → 0.764 |
| 100,000 | data / normal | 559.39 | 160.64 | 160.20 (2.53) | 1.00x | 15.259 → 7.630 |
| 100,000 | data / Student-t | 877.88 | 244.73 | 245.78 (3.64) | 1.00x | 15.259 → 7.630 |
| 1,000 | active / Student-t | 10.62 | 18.91 | 16.31 (0.57) | 1.16x | 0.176 → 0.100 |
| 10,000 | active / Student-t | 97.68 | 61.73 | 43.76 (1.89) | 1.41x | 1.755 → 0.993 |
| 100,000 | active / Student-t | 956.02 | 452.70 | 318.18 (17.43) | 1.42x | 17.548 → 9.919 |
| 10,000 | data / Student-t, P=4096 | 100.32 | 49.47 | 49.39 (2.95) | 1.00x | 1.776 → 1.013 |

The large active case is 3.00x faster than whole-slice execution, with 43.5% less
child arena storage than the original parallel prototype. Input copying drops
from 3,200,096 to 800,096 bytes per gradient. Shared merging drops to 96 bytes;
800,000 bytes of exclusive derivatives are published by workers. The returned
local-gradient vectors shrink by another 2,400,000 bytes, beyond the arena table.
Data-only arena storage halves without a discernible throughput improvement:
the removed storage was mostly cold. Small jobs still lose to serial execution.

Separate active-case phase medians: packing plus destination clearing falls
from 48.65 to 16.19 us; owner merging from 75.60 to 0.084 us. Execution, dispatch,
and worker publication measure 341.81 us for compact versus 370.73 us for full.
These diagnostic samples are not the main timing series and do not sum to its
median. The new opcode profile still attributes about 58% of child kernel time
to Student-t, 36% to mean arithmetic, and 6% to slice materialization/scatter.
No callback arithmetic was removed.

A predeclared worker/owner/owner/worker ablation at active N=100,000 holds the
compact graphs fixed. Ten samples per process give worker-publication medians
311.24 / 312.53 us and owner-only-merge medians 328.13 / 330.90 us. This supports
roughly another 5% benefit from publication on the workers, beyond compaction.
It is a separate measurement series, not additive to the main 1.42x gain.

## Historical verification (be0a0c8d)

- All 49 configurations pass both Release and ThreadSanitizer: empty, singleton,
  uneven and ordinary slices; normal and Student-t; active/data slices;
  1/2/4 threads, plus an 8-chunk/8-thread case. At three parameter points,
  compact serial and compact parallel agree bitwise with original chunks.
  Whole-slice and analytic comparisons retain `1e-10 + 1e-10*abs(reference)`;
  maximum absolute difference is 1.84e-9, only 0.000222 of the allowed bound.
- An unrelated arithmetic graph tests overlapping aliases, an interleaved data
  slot, a dead parameter block, fixed data ranges, and a whole-input consumer.
  Additional scalar and empty reads check fully compacted parameter blocks.
  Refusal cases cover opaque state, dynamic geometry/indexing, in-place writes,
  invalid ranges, stateful kernels, and parent offset overflow. Refusal leaves
  caller graph, fills, and map unchanged.
- Partial-overlap publication cases test span splitting and serial/parallel
  results for two callbacks that share only part of one imported parameter slot.
  Executor clones, repeated dispatch, deterministic error selection, and
  recovery after worker domain errors pass.
- TSAN stress runs 15,000 timed evaluations (five modes × three samples ×
  1000 iterations), including 3,000 compact parallel and 3,000 full parallel
  evaluations, with no diagnostics.
- Renaming callback/formal names passes; the existing reduce_sum and multichain
  regression tests pass. Formatting, Python syntax, and whitespace checks pass.

Timing evidence predates the final addition of scalar/empty/overflow checks and
an offset-overflow preparation guard, plus the two-job partial-overlap check. Those changes do not alter successful
warm evaluation code. Both binary identities and the final correctness/sanitizer
reruns are retained; the timing metadata identifies the measured executable.

## Historical decision and next integration work

Keep compact imports and exclusive publication. Defer a new shared arena/view
API: compact packing plus clearing is about 5% of the measured gradient, and
removing all of it is only a zero-cost ceiling, not a predicted gain. Eliminating
slice materialization and binding immutable inputs by view could help further,
but requires separate proof and measurement. The measured gain does not depend
on promising allocation-free gradients or behavior under memory pressure.

The next step is retained-reduction lowering with a reusable per-chain team,
serial execution for small workloads and unsupported callbacks, and explicit
values-only and reverse-seed handling. Reuse retained-loop trial lowering and
alias/activity rules. Keep general callback/effect handling, nested reductions,
public API controls, CmdStan parity, full inference, cross-platform validation,
and full integration suites as gates. The present probe does not implement
those production features.

## Reproduce

From the repository root with the pinned dependencies and existing Release /
TSAN configurations described in the stage-one record:

```sh
cmake --build build-reduce-sum --target bench_reduce_sum -j 8
python3 tools/bench_reduce_sum.py --imports --output build-reduce-sum/evidence/imports
build-reduce-sum/bench_reduce_sum --imports --n 100000 --active 1 --owner-merge
build-reduce-sum/bench_reduce_sum --imports --n 100000 --active 1 --check-only --profile
cmake --build build-reduce-sum-tsan --target bench_reduce_sum -j 8
python3 tools/bench_reduce_sum.py --imports --binary build-reduce-sum-tsan/bench_reduce_sum --checks-only --output build-reduce-sum/evidence/imports-tsan
build-reduce-sum-tsan/bench_reduce_sum --imports --n 31 --active 1 --iterations 1000 --samples 3
ctest --test-dir build-reduce-sum -R '^(test_reduce_sum|test_multichain)$' --output-on-failure
```

Omit `--imports` to retain the original three-mode probe. `--owner-merge` is an
ablation of publication location with the same compact imports. The target is
excluded from normal builds and installation.
