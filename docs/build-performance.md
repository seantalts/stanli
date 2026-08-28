# Build and pull-request latency

This is the reproducible baseline for build-time work. Keep local compilation,
runner queueing, required-check execution, and human review-to-merge latency as
separate measurements; combining them hides which change helped.

## Native baseline (2026-08-28)

Measured at `13f8c19d3f6f` on an Apple M3 Ultra with 32 CPU cores and 96 GiB
RAM, Apple Clang 21.0.0, CMake 4.3.3, and Ninja 1.13.2. These are clean Release
builds without ccache, embedded stanc, or an OCaml object.

| Graph | Parallel jobs | Clean build | Change from original `-j4` |
|---|---:|---:|---:|
| Original | 4 | 339.25 s | baseline |
| Shared-object graph | 4 | 215.57 s | -36.46% |
| Shared-object graph | 24 | 63.65 s | -81.24% (5.33x) |
| Shared-object graph | 32 | 64.03 s | -81.13% |

Twenty-four jobs is the RAM-aware default on this host and was 0.6% faster
than using all 32 cores. The original graph compiled the heavy runtime twice;
the shared-object graph reduced compile commands from 261 to 152 and Ninja
edges from 328 to 219. A no-op build takes 0.02 s. Touching `executor.cpp`
now compiles it once and relinks in 5.31 s; the old graph compiled it twice.

The first test run after linking is dominated by loading roughly a gigabyte of
test executables: 18.31 s at 24 jobs, versus 23.53 s for the original
sequential run. Once warm, the complete 67-test suite takes 2.2-2.5 s.

Reproduce the current measurement with:

```sh
./deps/fetch.sh
cmake -S . -B build-benchmark -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-benchmark --target clean
/usr/bin/time -lp cmake --build build-benchmark \
  --parallel "$(tools/build_jobs.sh)"
/usr/bin/time -lp ctest --test-dir build-benchmark \
  --parallel "$(tools/build_jobs.sh)" --output-on-failure
```

`tools/build_jobs.sh` caps work by both CPUs and memory/cgroup limits. Release
builds budget 4 GiB per job on macOS/Windows and 6 GiB elsewhere. Instrumented
builds should raise the budget; the documented ASan recipe uses 12 GiB/job.

## Required PR check baseline

Across the 21 successful PR runs immediately before this change, the required
`manylinux_2_28_x86_64` check had a 13.8-minute median and 37.7-minute p90.
Runner queueing was only 3 seconds median (34 seconds maximum); compile/cache
behavior, not queue capacity, dominated. CTest itself took 3-6 seconds. One
`web/index.html`-only PR waited 37.7 minutes, including 30.75 minutes in the
native compile step.

The workflow now gives allowlisted prose/browser-page changes a static path,
keeps unknown paths on the full path, reuses caches across compatible commits,
and preserves the exact required-check name through a final fail-closed gate.
Do not record an estimated CI win as a result. After at least 20 post-change
PRs, compare median/p75/p90 by change class and report runner queue, dependency
wait, execution, and review-to-merge latency separately.
