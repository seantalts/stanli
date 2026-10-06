# AVX2 copies of the dense-matrix kernels, chosen at run time

Status: prototype on `fastmath/avx2-kernels` (from `fastmath/base`), 2026-10-05.
Off by default (`-DSTANLI_AVX2_KERNELS=OFF`); the default build is unchanged.
Linux x86-64 and ELF/GNU ld only so far. A candidate for the fast mode in
[numerics-vs-speed](2026-10-05-numerics-vs-speed.md).

## What it is

`matrix_fns.cpp` and `matrix_solve.cpp` (cholesky, GP covariance, multivariate
densities, solves, wiener) are compiled twice into one library. The baseline
copy has its entry points renamed `*_base`. The AVX2 copy is built with
`-march=x86-64-v3` inside its own Eigen namespace (`-DEigen=EigenAvx2`).
`runtime/src/isa_dispatch.cpp` registers one of the two at start-up:

- default: the baseline copy, so default-mode results do not change;
- `STANLI_FAST_MATH=1`: the AVX2 copy, if the CPU runs x86-64-v3 (AVX2, FMA,
  BMI1/2, F16C, LZCNT, MOVBE, OS support for YMM state, via raw `cpuid` and
  `xgetbv`; clang's `__builtin_cpu_supports` rejects "lzcnt" and "movbe");
- `STANLI_ISA=baseline|avx2` overrides either way, for testing;
  `STANLI_ISA_VERBOSE=1` reports the choice.

## Why the whole-library v3 build is not the answer

Measured on the i9-13900K (AVX2 and FMA, no AVX-512), clang 18.1.3, Release;
the CI run in the numerics-vs-speed note (EPYC 7763) agrees on the replay:

| build | corpus replay failures (of 352) |
| --- | ---: |
| baseline | 3 (this toolchain; GP models) |
| whole library `-march=x86-64-v3` | 44 (CI run: 44) |
| AVX2 matrix kernels only | 13 (14 on an earlier main) |

CI also found 36 of 350 models more than 5% slower with the whole-library v3
build. Only the matrix kernels gain, so only they are built twice.

## Results (warm gradient, microseconds, mean of 4 interleaved rounds)

| model | plain baseline | new build, default | new build, `STANLI_FAST_MATH=1` |
| --- | ---: | ---: | ---: |
| gp (cholesky, N=200) | 1619 | 1620 | 916 (1.77x) |
| logistic (matvec, N=2000, K=20) | 43.6 | 44.0 | 43.5 |
| linreg_big, hier, elemwise | 718 / 33.0 / 561 | 697 / 32.5 / 562 | 703 / 32.7 / 560 |

Run-to-run spread 0.3-4.5%. The earlier spike measured gp at 1.56-1.77x
depending on how warm the machine was (absolute times drifted about 20% over
the session). With GCC 13 (spike build, not the in-tree one) gp was 1.77x too.
`logistic` gains 1.28x with the whole-library build, but its matvec lives in
`elementwise.cpp`; adding that file got 1.10x and cost 20 more replay
failures, so it is not included.

## The 10 new replay failures are not large errors

Fast mode fails 10 models that the baseline passes. The sensitivity analysis
below was done on the 11 of an earlier main; `s2_hurdle_cumulative`, one of
them, no longer fails on the current main (not investigated), and nothing new
appeared. For 10 of those 11 the AVX2-versus-baseline gradient differs by at
most 3.5 ULP of the largest gradient entry (mostly under 1 ULP); the 10-ULP gate trips because the gate is
per coordinate and those models have near-zero coordinates (`sw_cumulative`
coordinate 1 is 0.0070 beside entries near 125, a 2e-13 difference that counts
as 244,480 ULP). The last, `s2_gp_by_gr`, is ill-conditioned: the baseline
moves 8e-9 relative under a 4-ULP input perturbation and AVX2 differs by
2.7e-9. No model with a well-conditioned gradient deviates by more than a few
ULP of its largest entry. This is for the fast mode's own evidence; the
default gates are untouched.

## Rules this design had to follow (each one cost a failure to learn)

1. **Mixing AVX2 and baseline objects naively crashes** (`free(): invalid
   pointer` on 57 of 352 models). Eigen's templates have identical mangled
   names under both targets but different packet sizes and blocking, and the
   linker keeps one copy. Pinning `EIGEN_MAX_ALIGN_BYTES` did not help;
   `-DEigen=EigenAvx2` on the AVX2 translation units did.
2. **Shared inline functions that do not mention Eigen** (boost::math, the
   `wiener7_integrate` family, ...) still collide. The linker keeps the first
   definition it sees, so the AVX2 objects must come last, which makes the
   baseline copy win. Localizing the AVX2 symbols does not fix this: the AVX2
   copy of `DW.ref.__gxx_personality_v0` won and the link failed.
3. **Only in shared libraries.** A static archive's members are pulled in on
   demand, so the order is not ours. A static `stanli_check` kept the AVX2
   copy of the wiener gradient functions and moved `cm_ddm` by 1-3 ULP in
   default mode. The static `stanli` therefore carries no AVX2 objects and
   `isa_dispatch.cpp` falls back to baseline (weak reference).
4. **Static initializers** of the AVX2 objects run at load on every CPU (they
   zero stan-math globals with `ymm` stores), which would crash a baseline
   machine before dispatch. `objcopy` renames their `.init_array` to
   `avx2_init_array`; the dispatcher runs them only when AVX2 is selected.
5. The check is `tools/check_isa_baseline.py --map libstanli.so.map`: newer
   instructions are allowed only inside input sections of the AVX2 objects,
   and no load-time initializer may point into them. On the in-tree build:
   249,386 such instructions, all inside AVX2 objects; the same library fails
   the plain check. Both the order (rule 2) and the placement are verified from
   the linker map, so a regression fails loudly.

## Verification

- Default mode against the plain baseline build, `stanli_check` output for every
  model and point of the replay (`verify_refs.py` driving a wrapper that ran
  both binaries): identical, except `ch14_m14_10`, which is nondeterministic on
  main too (see below).
- Replay with the AVX2 objects linked last: default 3 failures (same set as the
  plain baseline), `STANLI_FAST_MATH=1` 13 failures (the spike's 14 without
  `s2_hurdle_cumulative`; nothing new), no
  crashes.
- Shared library, default mode versus plain baseline on `cm_ddm`: bitwise equal.
- `tests/test_isa_baseline.py`: 3 new tests for the map parsing and the split
  between allowed and leaked instructions.
- Compiler output of the clean build inspected: no C++ diagnostics.

## Not done, and not known

- **Windows and macOS.** mingw has no `.init_array` section rename and no
  linker map in this form; this needs a different mechanism and is the hard
  part of the original ask. Dispatch and the Eigen namespace are not Linux
  specific, but untested elsewhere.
- **No run on a CPU without AVX2.** There is no emulator here; safety on an old
  CPU rests on the placement check above. `qemu-user -cpu Nehalem` would test
  it directly.
- **GCC for the in-tree build.** The GCC result is from the spike build only.
- **Fidelity policy.** Fast mode gives different results on AVX2 and non-AVX2
  machines for the same seed, and 10 models would need documented exceptions
  to their 10-ULP gates, or fast mode a different gate (the plan in
  numerics-vs-speed already says high-precision reference with a scaled bound).
- **Binary size.** Carrying both copies makes the shared library 63.1 MB
  against 54.6 MB (+15%) for two files.
- **R/Python surface.** `STANLI_FAST_MATH` is an environment variable only.
- **AVX-512.** Not measured anywhere.

## A separate observation

`ch14_m14_10` (rethinking) is nondeterministic in the plain baseline build:
repeated identical `stanli_check` runs at point 0 produced 2 distinct outputs
across several batches (1 of 6, then 0 of 40, in different runs), and the same
second output appears with the new build. Not investigated here.

## Reproducing

```sh
cmake -S . -B build-fast -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DSTANLI_AVX2_KERNELS=ON \
  -DSTANLI_STANC_EMBED_OBJ=$PWD/deps/stanc3/stanc_embed.o -DSTANLI_OCAML_STDLIB=...
cmake --build build-fast --target stanli_shared
python3 tools/check_isa_baseline.py --map build-fast/libstanli.so.map build-fast/libstanli.so
STANLI_FAST_MATH=1 STANLI_ISA_VERBOSE=1 python3 ...   # use the library from Python or R
```
