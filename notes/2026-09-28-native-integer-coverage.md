# Native integer operations in the existing register engine

Ordinary runtime integer arithmetic no longer requires a MIR callback. The
register program now has integer addition, subtraction, multiplication,
negation and absolute value. Division and remainder reuse existing Stan Math
integer instructions. Integer sum uses integer additions, and standalone
integer returns reconstruct both public value representations.

Registers still store doubles, which represent every Stan int exactly. The
operations convert their operands to Stan's C++ integer type before computing
the result. This matches the upstream operation's width and prevents a real
expression consuming a widened integer result. As with stanc's generated C++,
signed overflow is not a portable numerical contract; the overflow probes only
compare this build's behavior with the existing interpreter. Independent oracle
claims use valid, representable arithmetic, including negative operands.

The new operations also have zero-derivative rules in the generated adjoint
engine. They clear overwritten adjoint cells and do not differentiate integer
inputs. This avoids forcing otherwise-supported real computations onto var
replay. Direct RK admission recognizes the new exact-value instructions; its
existing control-flow and other opcode restrictions remain.

## Evidence

The new ODE fixture selects an integer from state, performs repeated multiply,
add, subtract, division, remainder, negate, abs and array sum, then uses it in a
real derivative. Both density and output callbacks select register execution.
All 21 independently recorded CmdStan values at three points match exactly
(0 ULP). The native test suite passes all 295 tests; the complete recorded
corpus passes all 329 models / 1,020,194 values and 124 platform ULP gates.
Existing corpus exceptions remain, including a worst 7040-ULP cancellation
case, so this is not a universal ten-ULP claim.

Focused tests cover integer-returning standalone functions, negative division
and remainder, callback values/weighted gradients, and adjoint correctness when
integer operations overwrite an input and feed a real expression. Noncanonical
legacy C++ DataMap integer mirrors retain their established fallback.

## Native measurements

Six alternating fresh-process pairs against the preceding loop-semantics
checkpoint. Release, Darwin arm64, AppleClang 21; 200 ms warmup and 250 ms timed
windows. Public API phase measurements include call overhead; native gradients
use bench_grad. Numerical checksums agree before and after.

| Model | Preparation | Native warm gradient | 100 warmup + 100 draws and rows |
| --- | ---: | ---: | ---: |
| ode_integer_arithmetic | 291.0 → 324.1 µs | 251.057 → 4.014 µs | 337.049 → 4.879 ms |
| ode_branch_returns | 326.8 → 318.5 µs | 1.870 → 1.904 µs | 3.410 → 3.446 ms |

Whole-process RSS is approximately 28 MB and does not establish retained-memory
costs. Library sizes, hashes, phase medians/MAD and raw samples are in the
[performance artifact](2026-09-28-native-integer-performance.json). These are
coverage fixtures, not a broad model-throughput claim.

Remaining work includes callback container layouts, runtime integer/control
packing, dynamic selectors and mutation, and large constant-loop expansion.
The while/for binding-lifetime and truly changing-size-storage exclusions are
not removed by adding integer arithmetic.
