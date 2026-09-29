# Finalized integer-payload compaction

## Scope and proof

Baseline: `94c94e24d8056594e37b2f2b7a872e88b608e5cd`, including the merged
bind-only scratch metadata change. This change concerns Graph integer-payload
ownership only. Op, Slot, KernelCtx, kernel arithmetic, Program::Call, and
both execution sweeps are unchanged.

Construction/lowering keeps `vector<vector<int>>`: pending Ops in rewrite
passes can hold views outside `graph.ops`. Initial Executor construction is
the late boundary at which it is safe to rebind all owned operation views.
`Graph::compact_idata()` packs each referenced construction buffer once,
releases dead buffers and outer-vector capacity, and retains one immutable
shared owner. Executor copies share those integers but still bind independent
values, adjoints, scratch, and contexts. Graph gains a 16-byte shared_ptr;
on this host Graph is 120 bytes and Executor 352. Op remains 64, Slot 24,
and KernelCtx 312 bytes.

The structural predicate uses exact construction-buffer bases and valid
lengths, never source/model names or inferred pointer ranges. Any borrowed,
interior, or invalid view declines compaction for the entire graph. This
preserves constant-folding subgraphs that borrow parent metadata. Non-null
zero-length views are conservatively treated as views too. Custom opaque
payloads must own their integers, not borrow from the construction pool;
the in-tree ownership audit found no such borrowers.

All refusal checks and allocating work precede publication. After publication
only nonthrowing pointer assignments and vector swap remain. If no operation
has an integer view, the pool is released without allocating a lookup table.
Otherwise the temporary table is 24 bytes per construction buffer on this
host, with O(P log P + N log P + I) work for P buffers, N operations, and I
retained integers. Graph copying after finalization needs no integer rebinding
unless mutable arrays have subsequently been appended.

Finalization is one-shot once a compact owner exists. Copies/extensions of a
finalized Graph remain supported: they share its immutable buffer and retain
ordinary deep-copy/rebinding for appended mutable arrays. Re-finalization
does not compact those new arrays or invalidate existing finalized views.
An all-dead graph creates no shared owner, so later live additions can still
be compacted. Repacking extensions is deliberately deferred.

The new high-level tests cover differently sized and shared payloads, dead
arrays, empty graphs, repeated execution after every source owner dies,
parameter changes, copy assignment, and extension of finalized graphs.
Mixed borrowed/owned execution observes later updates to caller-owned data;
an interior-view graph remains executable when moved. An invalid length
declines, then succeeds after repair. No pointer-equality assertions are used.
Existing logistic, opaque-payload, density, and multithreaded pool tests cover
the wider executor lifecycle under the changed ownership policy.

## Evaluator and method

The predeclared gates are exact execution/lifetime correctness, lower retained
integer storage, lower multi-executor memory, no regression in eight-executor
total setup including compaction, and no repeatable gradient regression above
5%. Single-executor startup can become slower; it must be measured separately,
not hidden inside a faster clone number. A hot-loop speedup is not required.

Apple M3 Ultra, arm64, Apple clang 21.0.0. Separate clean Release builds use
`-O3 -DNDEBUG -ffp-contract=off`; baseline/candidate benchmark sources are
identical. Stan Math: `8f326d14599d3030c626c46532d8e8534c1cdbec`; Stan:
`c96d04115d35cb04f42e45c5a69a82f9704798f1`; stanc3:
`8c4b874cb8ab4519f9b66d9b788789d0b26b9544`.

The INDEX chain has one live one-integer payload per operation; ADD has none.
Optional dead payloads each contain four integers. Both have one parameter
and an exact analytical value/gradient oracle. Executor counts include the
original. The driver alternates AB/BA fresh processes, checks graph shape
and exact sinks, and reports paired medians and interquartile ranges over
12 pairs. There are 20 timed gradients per executor after one checked warmup;
no timing overlaps this task's builds or tests. Density/trivial `bench_opcost`
and compiled Eight Schools `bench_grad` are independent canaries.

RSS uses `mach_task_info`/`getrusage` in MiB. It includes allocator retention,
temporary high-water marks, and page rounding, not just live allocations.
Per-graph integer counters include shared storage: summing them over clones
would overcount physical storage. Timings are from a development machine
without CPU pinning or control of independent background activity. No x86-64
timing or hardware cache-miss measurement is claimed.

## Results

The final implementation passed **113/113 Release and 113/113 ASan** tests
(`ASAN_OPTIONS=detect_leaks=0`), after rebuilding all affected binaries.
Formatting, Python syntax, and diff checks passed. One-operation ADD/INDEX
boundaries passed with one/two executors and with dead arrays. All paired
benchmark sinks matched. The clone benchmark checks exact values/gradients;
the compiled-model canary prints a rounded sink and is not by itself a
bitwise-gradient oracle.

At 100,000 live one-integer payloads, retained payload buffers fall from
100,000 per executor to **one buffer shared across all clones**, holding
400,000 bytes of integers. In the live-plus-dead case, 200,000 construction
buffers holding 500,000 integers become one buffer holding 100,000 integers.
The all-dead case retains zero buffers/elements. This avoids the baseline's
24-byte vector object and individual allocation per payload per clone;
allocator bookkeeping and unused capacity add to those requested bytes.

Current RSS after cloning, median [Q1, Q3], MiB:

| Workload | Executors | Baseline | Candidate |
| --- | ---: | ---: | ---: |
| INDEX 25k | 8 | 98.375 [98.359, 98.481] | 91.188 [91.188, 91.188] |
| INDEX 25k | 32 | 385.516 [385.117, 385.516] | 355.203 [355.172, 355.219] |
| INDEX 100k | 8 | 385.555 [384.981, 386.707] | 355.899 [354.836, 356.297] |
| INDEX 100k + 100k dead | 8 | 421.742 [421.546, 421.754] | 364.641 [363.820, 365.422] |
| ADD 25k + 100k dead | 8 | 122.696 [121.672, 122.832] | 94.766 [94.359, 94.770] |

That is about 7-8% less total resident memory for the live-payload clone sets,
13.5% less with the extra dead arrays, and 22.8% less for the all-dead stress
case. These are payload-heavy synthetic graphs, not expected savings for
every model. The no-payload ADD controls are essentially unchanged in RSS.

Total setup includes construction, compaction, initial binding, and all
clones. Paired candidate/baseline ratios below are median [Q1, Q3]; lower
is better. Absolute setup values are medians in milliseconds.

| Workload / executors | Total setup A -> B | Setup ratio | Per-clone ratio | Gradient ratio |
| --- | ---: | --- | --- | --- |
| INDEX 25k / 8 | 14.661 -> 9.994 | 0.688 [0.672, 0.705] | 0.554 [0.540, 0.565] | 0.978 [0.946, 0.999] |
| INDEX 25k / 32 | 58.810 -> 34.307 | 0.581 [0.574, 0.588] | 0.549 [0.539, 0.554] | 0.975 [0.963, 0.982] |
| INDEX 100k / 8 | 64.810 -> 43.567 | 0.667 [0.661, 0.682] | 0.531 [0.525, 0.543] | 0.992 [0.975, 1.008] |
| INDEX 100k + 100k dead / 8 | 81.393 -> 48.999 | 0.596 [0.571, 0.611] | 0.432 [0.410, 0.441] | 0.981 [0.967, 0.995] |
| ADD 25k + 100k dead / 8 | 20.879 -> 10.466 | 0.504 [0.494, 0.514] | 0.390 [0.385, 0.398] | 1.004 [0.993, 1.082] |

At 100k live payloads / eight executors, each clone takes 8.484 -> 4.513 ms.
Cloning becomes 47% faster and total setup 33% faster, even including the
6.413 ms one-time compaction. At 25k live payloads, compaction is about
1.35 ms; scaling to 100k is consistent with the nonquadratic sorted-map
algorithm. Initial binding alone remains approximately unchanged.

### Single-executor tradeoff

This is not a universal RSS or startup improvement. One executor over 100k
live payloads takes 6.236 -> 12.681 ms total setup; compaction contributes
6.421 [6.281, 6.577] ms. Its RSS is 51.359 -> 51.711 MiB. With 100k additional
dead buffers, setup is 7.356 -> 16.918 ms and RSS 55.875 -> 60.844 MiB.
Freed pool and lookup-table pages can remain with malloc, so lower live
storage can coexist with a higher process high-water mark. Multi-executor
sharing amortizes that cost; a single short-lived evaluation may not.

The all-dead fast path avoids allocating the lookup table: finalization of
100k dead buffers takes 0.801 [0.754, 0.812] ms (including their destruction),
and single-executor RSS is 19.031 -> 18.984 MiB. An earlier prototype allocated
the map even here and used 21.297 MiB; that unnecessary allocation was removed
before the final comparisons. Ordinary graphs with an empty pool return
immediately, without scanning operations or allocating.

No-payload ADD gradient ratios at 1/8/32 executors are respectively
0.995 [0.987, 1.081], 1.020 [1.004, 1.046], and 0.998 [0.996, 1.003].
Its eight-executor total setup ratio is 1.012 [0.987, 1.035], effectively
unchanged rather than a pooling win. The primary payload-bearing setup and
memory gates pass; no repeatable gradient regression exceeds 5%.

Independent canaries, nanoseconds per complete evaluation, median [Q1, Q3]:

| Canary | Baseline | Candidate | Paired ratio [Q1, Q3] |
| --- | ---: | ---: | --- |
| Normal-density gradient | 72,354 [71,799, 72,942] | 72,271 [72,066, 72,806] | 0.995 [0.990, 1.006] |
| Normal-density forward | 51,729 [51,562, 52,072] | 51,526 [51,396, 51,960] | 0.995 [0.989, 1.000] |
| Trivial-op gradient | 29,555 [29,256, 30,103] | 29,822 [29,393, 30,055] | 1.011 [0.997, 1.019] |
| Eight Schools gradient | 292.75 [288.63, 295.23] | 289.15 [285.70, 293.63] | 0.990 [0.981, 1.004] |
| Eight Schools forward | 246.05 [245.08, 249.08] | 244.85 [239.58, 247.45] | 0.988 [0.972, 1.008] |

## Interpretation and limits

The main benefit is fewer allocations and cheaper multi-chain copies. Integer
data are also packed in first-use order, but kernel access still uses the
same raw pointer and KernelCtx layout. The small INDEX gradient gains do not
establish a general model-wide or architecture-wide speedup. Expected memory
and cloning benefits apply structurally to x86-64 too, but their magnitude
and cache effects need measurement on that platform.

The research protocol kept this experiment separate from context packing,
shared dispatch plans, and earlier CALL work. Its ownership audit dictated
late finalization and conservative fallback; its paired measurements exposed
and removed the avoidable all-dead lookup allocation. Repeated compaction of
extensions and reducing the mixed/live compaction temporary peak remain
possible later work, not hidden promises of this implementation.

Raw final samples are retained locally in `build-idata-candidate/`:
`index100k-final.json`, `index100k-dead100k-final.json`, `index25k-final.json`,
`add25k-final.json`, `add25k-dead100k-final.json`, and
`canary-idata-final.json`. Files without `-final` are preliminary measurements
before the all-dead fast path and must not be substituted for the final run.

## Reproduction

Build the baseline with the new benchmark sources but the baseline runtime,
before applying the runtime change; never rebuild its binaries afterwards.
Replace `/path/to/pinned/stanc` with the local pinned compiler.

```sh
cmake -S . -B build-idata-base -DCMAKE_BUILD_TYPE=Release \
  -DSTANLI_STANC_EXECUTABLE=/path/to/pinned/stanc
cmake --build build-idata-base -j4 \
  --target bench_executor_clone bench_opcost bench_grad
# Apply runtime/test changes, then configure fresh directories.
cmake -S . -B build-idata-candidate -DCMAKE_BUILD_TYPE=Release \
  -DSTANLI_STANC_EXECUTABLE=/path/to/pinned/stanc
cmake --build build-idata-candidate -j4
cmake --build build-idata-candidate -j4 --target bench_executor_clone
STANC=/path/to/pinned/stanc ctest --test-dir build-idata-candidate \
  --output-on-failure -j4

cmake -S . -B build-idata-asan -DCMAKE_BUILD_TYPE=None \
  -DCMAKE_C_FLAGS='-O1 -g1' -DCMAKE_CXX_FLAGS='-O1 -g1' \
  -DSTANLI_SANITIZE=address \
  -DSTANLI_STANC_EXECUTABLE=/path/to/pinned/stanc
cmake --build build-idata-asan -j4
ASAN_OPTIONS=detect_leaks=0 STANC=/path/to/pinned/stanc \
  ctest --test-dir build-idata-asan --output-on-failure -j4

python3 tools/bench_executor_clone.py \
  build-idata-base/bench_executor_clone build-idata-candidate/bench_executor_clone \
  --workload index --ops 100000 --executors 1 8 --samples 12 --reps 20 \
  --json build-idata-candidate/index100k-final.json
# Repeat with --dead-payloads 100000; also run 25k INDEX at 1/8/32 executors,
# 25k ADD with/without dead payloads, and one-operation correctness boundaries.

# Run both canaries in each build directory, alternating order over 12 pairs.
build-idata-candidate/bench_opcost
build-idata-candidate/bench_grad \
  tests/fixtures/es.tmir.sexp tests/fixtures/eight_schools.json 500000
```

The separate `STANC` environment variable is required for source-level lit
tests in this managed worktree, which lacks the usual `deps/stanc3` symlink.
The CMake setting controls fixture generation, not runtime compiler lookup.
