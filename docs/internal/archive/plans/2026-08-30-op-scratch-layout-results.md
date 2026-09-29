# Bind-only Op scratch layout

## Scope and proof

Baseline: `a0dd9d86e989a8965994de2b2761481d5e00c858`. The candidate removes
`Op::scratch_off` and `Op::scratch_len`; it does not change `KernelCtx`,
`Slot`, the dispatch tables, `Program::Call`, or either execution sweep.

Scratch sizing still visits operations in graph order and invokes each
kernel's sizing callback exactly once. A bind-local offset vector retains
the starts until the complete scratch arena is allocated. Contexts then
receive their final pointers, and the offset vector dies. An Executor copy
repeats binding into its own arena. Zero-sized windows preserve their place
in the prefix sum; an entirely empty scratch arena yields a null pointer.
No kernel arithmetic, model recognition, lowering eligibility, or fallback
rule changes.

The new black-box Executor test mixes scalar and vector normal densities
with an intervening scratch-free ADD. It checks exact analytical gradients,
clones the Executor, destroys the source, interleaves value-only and gradient
evaluations eight times, and changes the clone's parameters. Existing tests
also cover empty/zero-op graphs, integer and opaque payload lifetimes, and
multithreaded pool reuse. No new pointer-identity assertions are needed.

The primary performance gate is lower live metadata storage. A repeatable
regression greater than 5% in binding, cloning, or gradients is a provisional
investigation threshold, not a claimed timing improvement. The competing
predictions are cheaper graph copies from fewer bytes versus extra bind-time
allocation from the temporary offset vector. Hot gradient work is unchanged.

## Measurement method

Apple M3 Ultra, arm64, Apple clang 21.0.0. Separate clean Release builds use
`-O3 -DNDEBUG -ffp-contract=off`. Stan Math is `8f326d14`; Stan is `c96d04115`.
The baseline and candidate benchmark sources are identical. No timing run
overlaps this task's builds or test runs.

`bench_executor_clone` uses a scalar ADD chain with N operations, exactly
N reserved operation records, N+2 slots/elements, and one parameter. Executor
counts include the original. It checks each executor's exact result and
gradient before timing repeated evaluations. The Python driver alternates
AB/BA fresh processes, checks graph shape and numerical sink parity, and
reports medians and interquartile ranges over 12 pairs. The primary size is
25,000 operations at 1/8/32 executors; 100,000 operations checks scaling.
One-operation graphs are a correctness boundary, not a timing claim.

The canaries are `bench_opcost` (scalar normal densities and trivial ops)
and compiled non-centered Eight Schools via `bench_grad`. RSS is measured
with `mach_task_info` and `getrusage`, in MiB. It includes allocator retention
and page rounding, so exact live-storage savings are reported separately.
There is no x86-64 timing measurement in this experiment.

## Results

Both complete configured suites passed: **113/113 Release and 113/113
AddressSanitizer** (`ASAN_OPTIONS=detect_leaks=0`). Formatting and diff checks
passed. The one-operation benchmark boundary also passed with one and two
executors. Every benchmark pair had matching sinks; the clone benchmark's
unrounded value and gradient checks were exact. The compiled-model timing
tool prints a rounded sink, so that canary alone is not a bitwise-gradient
oracle; the lifecycle and model tests supply the exact clone checks.

`sizeof(Op)` fell **80 -> 64 bytes**, a 20% reduction in operation-record
storage. `sizeof(Slot)` stayed 24; `sizeof(KernelCtx)` stayed 312. Each
25,000-op executor saves exactly 400,000 live Op bytes; each 100,000-op
executor saves 1,600,000 bytes. The temporary bind array costs 8 bytes/op
only while binding, and its freed allocation can remain resident.

Current RSS after cloning, median [Q1, Q3], in MiB:

| Ops | Executors | Baseline | Candidate |
| ---: | ---: | ---: | ---: |
| 25,000 | 1 | 13.750 [13.750, 13.750] | 13.516 [13.516, 13.531] |
| 25,000 | 8 | 93.391 [93.391, 93.391] | 90.422 [90.422, 90.438] |
| 25,000 | 32 | 366.422 [366.422, 366.473] | 354.453 [354.047, 354.469] |
| 100,000 | 1 | 48.156 [48.156, 48.156] | 47.375 [47.375, 47.375] |
| 100,000 | 8 | 364.125 [364.125, 364.125] | 352.609 [352.609, 352.609] |

The large clone sets use about **3.2-3.3% less total resident memory**.
This is not a 20% reduction in total Executor memory: contexts and numerical
arenas still dominate. Peak RSS follows current RSS within 0.032 MiB here.

Timing ratios, candidate/baseline paired median [Q1, Q3]; lower is better:

| Ops / executors | Graph construction | Binding | Per clone | Gradient |
| --- | --- | --- | --- | --- |
| 25k / 1 | 0.902 [0.861, 0.964] | 1.038 [1.009, 1.086] | n/a | 1.027 [0.997, 1.059] |
| 25k / 8 | 0.882 [0.838, 0.963] | 1.047 [0.966, 1.080] | 0.992 [0.943, 1.009] | 0.984 [0.957, 1.019] |
| 25k / 32 | 0.882 [0.854, 0.899] | 1.004 [0.996, 1.015] | 0.980 [0.968, 1.010] | 1.001 [0.998, 1.005] |
| 100k / 1 | 0.882 [0.867, 0.900] | 1.000 [0.989, 1.032] | n/a | 1.035 [0.976, 1.053] |
| 100k / 8 | 0.889 [0.847, 0.939] | 0.997 [0.985, 1.010] | 0.979 [0.973, 0.984] | 1.000 [0.998, 1.003] |

Absolute medians at 100k ops / eight executors: graph construction
0.952 -> 0.839 ms, binding 3.189 -> 3.200 ms, per clone 3.760 -> 3.689 ms,
and gradient 1.779 -> 1.778 ms. Graph construction benefits consistently
from smaller records in this synthetic workload. Cloning improves modestly;
binding is roughly flat to 5% slower at the smaller size, with overlapping
sample ranges. No repeatable regression exceeded the investigation gate.

Canary times are nanoseconds per complete graph evaluation, median [Q1, Q3]:

| Canary | Baseline | Candidate | Paired ratio [Q1, Q3] |
| --- | ---: | ---: | ---: |
| Density gradient, 4,916 ops | 70,006 [69,748, 70,926] | 70,619 [70,185, 71,128] | 1.008 [0.991, 1.016] |
| Density forward | 50,359 [49,954, 50,904] | 50,893 [50,378, 51,223] | 1.010 [0.994, 1.027] |
| Trivial gradient, 4,916 ops | 28,267 [27,927, 29,690] | 28,002 [27,909, 28,491] | 0.992 [0.943, 1.008] |
| Eight Schools gradient | 272.8 [269.7, 274.0] | 275.0 [272.3, 276.2] | 1.007 [0.996, 1.013] |
| Eight Schools forward | 233.0 [232.1, 234.5] | 234.7 [232.6, 236.2] | 1.008 [0.990, 1.014] |

These results support a memory/graph-copy change, **not a hot-loop speedup**.
The roughly 1% canary differences and mixed small single-executor differences
do not establish a general gradient performance effect. Timings were taken
on a development host, without CPU pinning; independent background activity
was not controlled. Repeated fresh-process pairing limits drift but does not
replace a dedicated performance machine.

Raw local samples are retained in `build-op-layout-candidate/`:
`clone-layout-25k.json`, `clone-layout-100k.json`, and `canary-layout.json`.

## Reproducing

Configure the baseline before applying the runtime change, using the same
benchmark sources on both sides. Do not rebuild its binaries afterwards.
Replace the compiler path below with the pinned stanc in the local checkout.

```sh
cmake -S . -B build-op-layout-base -DCMAKE_BUILD_TYPE=Release \
  -DSTANLI_STANC_EXECUTABLE=/path/to/pinned/stanc
cmake --build build-op-layout-base -j4 \
  --target bench_executor_clone bench_opcost bench_grad
# Apply the runtime change, then build in a new directory.
cmake -S . -B build-op-layout-candidate -DCMAKE_BUILD_TYPE=Release \
  -DSTANLI_STANC_EXECUTABLE=/path/to/pinned/stanc
cmake --build build-op-layout-candidate -j4
cmake --build build-op-layout-candidate -j4 --target bench_executor_clone
STANC=/path/to/pinned/stanc ctest --test-dir build-op-layout-candidate \
  --output-on-failure -j4

cmake -S . -B build-op-layout-asan -DCMAKE_BUILD_TYPE=None \
  -DCMAKE_C_FLAGS='-O1 -g1' -DCMAKE_CXX_FLAGS='-O1 -g1' \
  -DSTANLI_SANITIZE=address \
  -DSTANLI_STANC_EXECUTABLE=/path/to/pinned/stanc
cmake --build build-op-layout-asan -j4
ASAN_OPTIONS=detect_leaks=0 STANC=/path/to/pinned/stanc \
  ctest --test-dir build-op-layout-asan --output-on-failure -j4

python3 tools/bench_executor_clone.py \
  build-op-layout-base/bench_executor_clone \
  build-op-layout-candidate/bench_executor_clone \
  --ops 25000 --executors 1 8 32 --samples 12 --reps 20 \
  --json build-op-layout-candidate/clone-layout-25k.json
python3 tools/bench_executor_clone.py \
  build-op-layout-base/bench_executor_clone \
  build-op-layout-candidate/bench_executor_clone \
  --ops 100000 --executors 1 8 --samples 12 --reps 20 \
  --json build-op-layout-candidate/clone-layout-100k.json

# Run each canary in both build directories, alternating order over 12 pairs.
build-op-layout-candidate/bench_opcost
build-op-layout-candidate/bench_grad \
  tests/fixtures/es.tmir.sexp tests/fixtures/eight_schools.json 500000
```

The source-level lit tests need `STANC` here because the managed worktree
does not contain the usual `deps/stanc3` symlink. The CMake compiler setting
above controls fixture generation, not `stanli_check`'s runtime lookup.

## Deferred work

Flattened integer payload storage and shared immutable binding metadata
remain separate experiments. KernelCtx packing is also deferred: this
change deliberately does not claim better hot-context cache locality.
