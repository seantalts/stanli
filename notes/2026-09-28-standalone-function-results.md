# Compiled standalone functions: implementation and measurements

Current authorization remains native performance and interpreter-gap work until
completion or a consequential user decision. Instruction generation and stencil
JIT remain tabled. Upstream base: 5b913866768f351f86eed4d5a7246414530f3b2d;
matched standalone baseline: 304551f0.

Eligible real-valued standalone calls now reuse the shared register compiler.
Plans specialize on the selected definition, complete logical dimensions and
integer argument contents; real argument values remain runtime inputs. Each
invocation owns its registers. Immutable cached plans are shared safely across
concurrent calls. Full-key equality determines hits. No execution error retries
through the interpreter, preserving print/reject sequencing.

The cache retains at most eight entries and four MiB of estimated program,
register and key storage. This is not a whole-process memory bound: owned
metadata and concurrent in-flight plans have additional cost. After saturation,
a missing signature must recur on consecutive misses before compilation. The
initial FIFO implementation regressed rotating-signature calls; the admission
correction avoids repeated compilation while admitting newly stable workloads.

Safe refusal remains for runtime-shaped results, nested array layouts,
recursion, integer-result/unsupported integer arithmetic contracts, void calls,
unseeded RNG and other unsupported compiler operations. Noncanonical integer
mirrors in the C++ data API bypass the cache. Proven real return types clear
stale integer mirrors consistently on both engines. This does not remove the
interpreter or claim full standalone language coverage.

Fable's [implementation review](2026-09-28-fable-standalone-implementation-review.md)
identified ordinary exceptions from speculative constant evaluation. A negative
constructor extent in an untaken branch reproduced the defect. Both standalone
and solver callback compilation now treat such exceptions as pre-execution
refusals; allocation failure propagates. Regression tests verify the untaken
branch succeeds and the taken branch emits its effect exactly once before
rejecting. [Plan and evaluator](2026-09-28-standalone-function-plan.md).

## Final measurements

Six counterbalanced fresh-process pairs per function, Release Apple Clang 21
arm64, public C ABI through ctypes, 200 ms warmup and 250 ms measurement per
phase. ctypes and callback overhead are included; the result writer does no
copying. Values below are medians in microseconds (MAD in parentheses).

| Function | Warm baseline | Warm candidate | First baseline | First candidate |
| --- | ---: | ---: | ---: | ---: |
| affine, vector length 32 | 9.261 (0.214) | 1.640 (0.053) | 161.9 (3.0) | 178.5 (8.1) |
| branch/loop early exit | 12.283 (0.184) | 1.114 (0.037) | 152.6 (3.4) | 198.8 (6.0) |
| integer-specialized vector, N=8 | 13.539 (0.250) | 1.367 (0.020) | 173.4 (4.5) | 176.5 (8.2) |

Repeated calls improve 5.6–11.0x in these fixtures. The cold branch call costs
about 46 microseconds more; this is a real compilation tradeoff. MIR handle
creation stays around 193–202 microseconds. Rotating twelve integer signatures
takes 12.875 (0.284) versus 6.226 (0.084) microseconds; this includes interpreted
misses and retained compiled hits, not universal compiled coverage.

Peak process RSS is approximately 16.6–17.1 MB. The sized/churn candidate adds
278,528 bytes at the median; the other fixtures are within about 66 KB. Retained
cache payload is bounded as described above, not independently isolated by an
allocator profiler. Library size changes from 40,051,008 to 40,101,760 bytes;
gzip from 12,360,522 to 12,385,847 bytes. No dependency was added.

These are standalone function-call measurements, not model gradients or full
inference. Raw samples, dispersion, input hashes and library hashes are in
[final evidence](2026-09-28-standalone-function-performance.json); the
[initial cache experiment](2026-09-28-standalone-function-initial-performance.json)
is retained, including its churn regression. Reproduce with
`tools/bench_function_paths.py LIBRARY affine|branch_exit|sized` in alternating
baseline/candidate processes.

## Validation and continuation

- Full current native suite: 277/277 passed.
- Independent pinned CmdStan 2.40 standalone API reference: 40 values over
  three parameter points and a repeated point, maximum one ULP; compiled
  selection and zero MIR events verified.
- Complete recorded corpus: 329/329, three points, 1,020,194 values, all 124
  model ULP gates passed. Worst scaled discrepancy 9.38e-13 / 7040 ULP; this
  aggregate gate does not establish a universal ten-ULP bound. Final subsequent
  edits were confined to standalone admission/output contracts and their tests.
- Installed Python and R interface suites pass with the final library.
- Tests cover zero extents, both branch outcomes, changing real values,
  integer specializations and boundaries, effects, layout refusals, concurrent
  cache use, overload validation and public callback writers.

Fable [reviewed the next-step ordering](2026-09-28-fable-next-runtime-review.md):
native performance already selects real-model kernel profiling; a general
dynamic-value runtime is a later architecture project, not an approval blocker.
Continue the [native-performance plan](2026-09-28-next-runtime-design-decision.md)
and select the next kernel from measured cost.
