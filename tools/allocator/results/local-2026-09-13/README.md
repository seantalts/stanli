# Matched local allocator rollout evidence

See the [interpretation and caveats](../../../../docs/allocator-rollout.md) and
[predeclared plan](../../../../docs/allocator-rollout-plan.md). Ratios are
baseline/candidate times (>1 improves throughput). Ranges are round ranges,
not confidence intervals. All recorded paths refer to the retained experiment
worktrees; substitute local paths when reproducing.

- `screen-*`: 52 cells, 2,080 timed processes, five paired rounds, both identical
  aliases. `confirmation-*`: all three cells below 0.97 plus three predeclared
  controls, seven rounds, 336 timed processes. No further local timing reruns.
- `*-runs.jsonl.gz` retain every process, command, repetitions, three block
  samples (native), complete-snapshot hash, graph dimensions, and peak RSS.
  `*-output.jsonl.gz` retain each process's stdout/stderr.
- `complete-snapshots.tar.gz` stores every unique model/worker full snapshot.
  All candidate/timed snapshots matched these exact bytes. `frozen-inputs.json`
  records the pinned public corpus, generated fixtures, and data/source/MIR hashes.
- `refactor-oracle.json` records rebuilding the retained evaluator on the
  current baseline objects; all 23 models matched the new evaluator.
- `cli-*`: the existing unmodified static CLI, SYSTEM in both builds, loops
  OFF/ON. 23 calibration + 460 timing processes. The A/A shifts make this
  screen inconclusive. Do not read it as a CmdStan or allocator comparison.
- `retention-*`: 1,179,648 gradients per variant, 12 small/large/small model
  sequences in each persistent NumPy host. All snapshots match.
- `build-audit.json`: compiler flags, identical dependency objects, linked
  loop words/addresses, and shipping/test/CLI sizes. `hosts-*`, suite logs,
  and `postformat-*` record the final-source integration verification separately
  from the timed source. Post-format timings are diagnostic only.
- `sha256.json` hashes the retained data files (this README is explanatory).

## Reproduce

Fetch the pinned dependencies with `./deps/fetch.sh` and build the matching
embedded compiler as documented in `TESTING.md`. Use Release builds on the
same machine and stop other builds/tests during the timed phases. Fetch
posteriordb at the pin in `prepare.py`.

```sh
cmake -S . -B build-system -DCMAKE_BUILD_TYPE=Release \
  -DSTANLI_SHARED_ALLOCATOR=SYSTEM -DSTANLI_ALIGN_LOOPS=OFF \
  -DSTANLI_BUILD_ALLOCATOR_BENCHMARK=ON
cmake -S . -B build-private -DCMAKE_BUILD_TYPE=Release \
  -DSTANLI_SHARED_ALLOCATOR=MIMALLOC -DSTANLI_ALIGN_LOOPS=AUTO \
  -DSTANLI_BUILD_ALLOCATOR_BENCHMARK=ON
cmake --build build-system --target stanli_allocator_benchmark --parallel 4
cmake --build build-private --target stanli_allocator_benchmark --parallel 4
python3 tools/allocator/prepare.py --pdb deps/posteriordb --output frozen-models \
  --library build-system/libstanli_allocator_benchmark.dylib
python3 tools/allocator/bench.py verify --eight --inputs frozen-models/models.json \
  --system build-system/libstanli_allocator_benchmark.dylib \
  --candidate build-private/libstanli_allocator_benchmark.dylib --output measurements
python3 tools/allocator/bench.py bench --eight --inputs frozen-models/models.json \
  --system build-system/libstanli_allocator_benchmark.dylib \
  --candidate build-private/libstanli_allocator_benchmark.dylib --output measurements
python3 tools/allocator/confirm.py --screen measurements \
  --inputs frozen-models/models.json --output confirmation
```

Supply the normal embedded-object/OCaml CMake arguments for your environment;
they are omitted above because their paths are machine-specific. Linux uses
`.so`; Windows uses `stanli_allocator_benchmark.dll` and `prepare.py --compiler`
with the shipping `stanli-compile.exe`. `ci.py` automates the paired build/check
from an existing native shipping build. These commands intentionally refuse
existing measurement directories. Do not change binaries after calibration.
