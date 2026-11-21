# Build options that no CI job builds (audit of 2026-11-20)

This records, for four build options, where each is defined, what uses it,
whether any workflow builds it, and what to do about it. It was checked on
tree `e283df8ff` by searching `.github/`, `tools/`, `CMakeLists.txt`, `r/`
and `python/` with `git grep`. Nothing was deleted or changed for this note.

A workflow "builds" an option when a `-D` flag, an environment variable or a
script that a workflow runs turns it on. The wheel job's check that the
shipped library has no AVX2 instructions asks the opposite question and does
not count.

## AVX2 kernels (`STANLI_AVX2_KERNELS`)

- **Defined**: `CMakeLists.txt:458` (the option, default OFF). The block at
  `CMakeLists.txt:436-499` compiles `matrix_fns.cpp` and `matrix_solve.cpp` a
  second time with `-march=x86-64-v3` in a renamed Eigen namespace, and
  renames their initializers with `objcopy`. It is linked in at
  `CMakeLists.txt:523-526`, `634-635` and `692-693`.
- **Used by**: `runtime/src/isa_dispatch.cpp` (98 lines, compiled only with
  this option) and the `--map` mode of `tools/check_isa_baseline.py`, which
  has tests in `tests/test_isa_baseline.py`. Nothing else reads it. The
  measurements are in `notes/performance/2026-10-05-avx2-kernel-dispatch.md`:
  a 1.77x speedup on one Cholesky GP (N=200), nothing measured on the other
  models, 8.5 MB more library, and 13 of 352 corpus replay failures (10 new).
- **Built by a workflow**: no. `git grep AVX2_KERNELS -- .github tools`
  finds nothing. `wheels.yml:724` runs `tools/check_isa_baseline.py` on the
  shipped library in its plain mode only.
- **Recommendation**: delete the option, `isa_dispatch.cpp`, and the `--map`
  mode of the checker with its tests. The code has a measured gain on one
  model and no coverage. If it stays, add one post-merge job on the manylinux
  x86-64 leg that configures `-DSTANLI_AVX2_KERNELS=ON`, runs
  `tools/check_isa_baseline.py --map`, and replays the corpus with
  `STANLI_ISA=avx2`.

## `STANLI_ISA`

- **Defined**: `runtime/src/isa_dispatch.cpp:69`, a raw `getenv`, not through
  `debug_env()`. `CMakeLists.txt:176` and `docs/hacking.md:813` list it among
  the variables that are not developer switches.
- **Used by**: only the AVX2 build above. Without
  `STANLI_AVX2_KERNELS=ON` the file is not compiled and the variable does
  nothing. `STANLI_ISA_VERBOSE` is read in the same file.
- **Built by a workflow**: no, for the same reason.
- **Recommendation**: delete it together with the AVX2 option. If the AVX2
  job above is added instead, put the `STANLI_ISA=avx2` corpus replay in that
  job.

## UBSan (`STANLI_SANITIZE=undefined`)

- **Defined**: `CMakeLists.txt:62-76`. `STANLI_SANITIZE` passes its value to
  `-fsanitize=`, and the `undefined` branch at `CMakeLists.txt:73-75` adds
  `-fno-sanitize-recover=undefined` so that a finding fails the test. The
  comment at `CMakeLists.txt:55-61` says `address,undefined` was clean on
  Apple clang 17, arm64, on 51 tests (the suite now has 578).
- **Used by**: `TESTING.md:512` and `docs/hacking.md:863` show the
  `address` form only.
- **Built by a workflow**: no. `wheels.yml:1707` passes
  `-DSTANLI_SANITIZE=address` and `wheels.yml:1817` passes `thread`. Nothing
  passes `undefined`.
- **Recommendation**: add `undefined` to the post-merge ASan job
  (`wheels.yml:1707`: `-DSTANLI_SANITIZE=address,undefined`). It reuses the
  same build, so it adds run time for the tests and no second compile. The
  first Linux run may report findings that the Mac run did not, so try it on
  a branch first. If nobody wants that, delete the branch.

## Native builds without threads (`STANLI_THREADS=OFF`)

- **Defined**: `CMakeLists.txt:214-228`. It controls `STAN_THREADS`, which
  makes Stan Math's autodiff stack thread-local. The build id gets a
  `-threads` suffix at `CMakeLists.txt:316`, and `test_reduce_sum_cli` is
  registered only when it is ON (`CMakeLists.txt:1631`). Code that checks
  `STAN_THREADS`: `runtime/include/stanli/threading.hpp`,
  `runtime/src/reduce_sum.cpp:22` and `runtime/src/lower_higher_order.cpp:721`.
- **Used by**: the browser builds, which are the only configurations without
  threads that CI builds, because `CMakeLists.txt:217` leaves `STAN_THREADS`
  off under Emscripten. `python/stanli/__init__.py:1025`, the R package and
  `tests/test_capi.cpp` ask `stanli_thread_safe()` and skip or run one thread
  when it is false. The downstream stanr package compiles the runtime
  sources itself (`.github/workflows/stanr.yml`), so it can build without
  threads; what it defines was not checked here.
- **Built by a workflow**: not natively. No workflow passes
  `-DSTANLI_THREADS=OFF`. The `no-stdio` job builds `stanli_runtime_objects`
  through `tools/check_no_stdio.sh`, which does not set the option, so it
  builds with threads.
- **Recommendation**: add `-DSTANLI_THREADS=OFF` to the `cmake` call in
  `tools/check_no_stdio.sh`. That post-merge job already compiles every
  runtime object, so it would then also compile the native no-threads
  configuration, without a new job. If stanr's own build of the runtime also
  leaves out `STAN_THREADS` (not checked here), that job would match it too.
