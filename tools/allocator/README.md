# Native warm-gradient measurements

This is a non-installed measurement target, separate from the
[opt-in private allocator](../../docs/private-allocator.md). It adds no
shipping warmup, sleeps, allocator tuning, loop alignment or public API.

The evaluator uses the shipping runtime objects and allocator boundary, calls
the complete native sampler gradient, and keeps worker threads and their Stan
tapes alive across blocks. It records preparation, first-gradient time,
warmup duration/count, timed aggregate throughput and memory separately.
Each worker warms with **at least 500 ms of actual gradients** before timing;
clock checks are between batches, outside timed blocks. Warmup is not part
of reported timed throughput, and first-gradient costs are not discarded.

`STANLI_ALLOCATOR_BENCH_WARMUP_MS=0` explicitly restores the historical short
mode (up to 1000 gradients or roughly 20 ms, with at least 32 iterations).
This is a diagnostic, not the standard warm metric. Direct workers accept
durations from 0 to 5000 ms. The matched driver clears allocator/Stanli
environment overrides and uses the default; never compare its results to a
different warmup boundary without labeling the difference.

## Build and correctness gate

With the repository's dependencies and pinned compiler configured:

```sh
cmake -B build-system -DCMAKE_BUILD_TYPE=Release \
  -DSTANLI_SHARED_ALLOCATOR=SYSTEM -DSTANLI_BUILD_ALLOCATOR_BENCHMARK=ON
cmake --build build-system --parallel 4
ctest --test-dir build-system -R test_allocator_benchmark --output-on-failure

cmake -B build-private -DCMAKE_BUILD_TYPE=Release \
  -DSTANLI_SHARED_ALLOCATOR=MIMALLOC -DSTANLI_BUILD_ALLOCATOR_BENCHMARK=ON
cmake --build build-private --parallel 4
ctest --test-dir build-private -R test_allocator_benchmark --output-on-failure
```

Normal native wheel validation enables the non-installed target and runs its
regression test. The test uses only Python's standard library, verifies the
warmup floor/default/short/refusal behavior with one/four workers, compares
every log density and gradient bit plus graph dimensions, and checks that
the benchmark exports are absent from the shipping library. It imposes no
speed threshold on shared CI hosts.

## Matched experiment

`prepare.py` freezes 16 posteriordb models and seven normal/gamma scaling
fixtures, using the recorded posteriordb pin and the shipping embedded
compiler (`--library`) or portable compiler (`--compiler`). It records
compilation time and source/data/MIR/compiler hashes.

Use a Python environment with NumPy. For example, on macOS, after building
with the embedded compiler (use `.so` on Linux and `.dll` on Windows):

```sh
python tools/allocator/prepare.py --pdb deps/posteriordb \
  --library build-system/libstanli.dylib --output measurements/inputs
python tools/allocator/bench.py verify \
  --inputs measurements/inputs/models.json --output measurements/run \
  --system build-system/libstanli_allocator_benchmark.dylib \
  --candidate build-private/libstanli_allocator_benchmark.dylib --eight
python tools/allocator/bench.py bench \
  --inputs measurements/inputs/models.json --output measurements/run \
  --system build-system/libstanli_allocator_benchmark.dylib \
  --candidate build-private/libstanli_allocator_benchmark.dylib --eight
```

The driver loads NumPy before the DSO and verifies old/new host buffers stay
outside the private heap. It compares complete snapshots at eight changing
parameter points, before/after repetition and against serial replay, plus
graph operations, slots and storage sizes. Calibration uses SYSTEM only.
The five-round schedule counterbalances two processes per slot and includes
identical-binary A/A aliases for **both** variants; each process has three
timed blocks. Outputs include raw stdout/stderr, snapshots, commands, hashes,
frozen calibration/schedule and all per-round ratios. Existing output
directories are refused.

`--cells cells.json` selects a predeclared list of `[model, workers]` pairs.
`confirm.py` performs one seven-round confirmation of all screen ratios
below 0.97 plus three fixed controls; 0.97 is a confirmation trigger, not an
acceptable regression budget. `retention.py` tests twelve fixed
small/large/small lifetimes without collection or tuning. Neither replaces
end-to-end sampling or the existing CLI/CmdStan comparison boundary.

## Why the warmup changed

The [retained investigation](https://github.com/seantalts/stanli/blob/4e2d1bb4da8b7b602c73baaab875bf78f5c7b439/docs/allocator-regression-investigation.md)
reproduced startup-sensitive Linux small-model parallel losses and their
recovery after sustained native work in the same DSOs. It did not establish
a shipping allocator fix or attribute the whole effect to OpenBLAS.
Apple ARM's large-vector loss remained unresolved.

See [benchmark context](../../docs/benchmarks.md#private-allocator-research)
and the immutable [raw evidence index](https://github.com/seantalts/stanli/blob/4e2d1bb4da8b7b602c73baaab875bf78f5c7b439/tools/allocator/results/README.md).
Those data used the archived evaluator; removing its placement telemetry and
unused diagnostic modes changes the source identity. New comparisons must
record fresh binaries; historical tables are not relabeled as measurements
of this cleaned harness.
