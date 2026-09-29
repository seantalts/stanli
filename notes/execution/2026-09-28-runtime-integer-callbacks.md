# Runtime integer callback arguments in generated quantities

Generated quantities can pass current-draw scalar integers and fixed-shape
integer arrays to modern ODE, DAE, algebra, quadrature, and adjoint ODE
callbacks. The existing theta buffer carries those values beside real
arguments. Kernel activity remains zero for generated quantities, so integer
lanes do not create solver sensitivities. Known integers remain specialized.

The register compiler binds runtime integers to the same fixed ranges it uses
for real inputs. The retained interpreter adapter reconstructs integer values
and serialized array order when a callback body still requires interpretation.
This adds no new engine and does not promote active-density integer arguments
into an autodiff parameter vector. Runtime-dependent callback dimensions still
require a local fallback.

Constant probing consults the register compiler's current bindings, not only
the enclosing graph's old preparation values. Writes invalidate the imported
constant status. External integer-array probing also refuses expressions that
refer to local runtime bindings after known constants have been substituted.
This matters when a branch changes an integer immediately before calling a
solver.

## Validation

A clean Release build was followed by focused callback, cross-path, and MIR
conformance tests, then all 301 native CTests and the complete recorded corpus:
329 models, 1,020,194 values, 124 same-platform ULP gates. All passed. The corpus
keeps its existing scaled-error and near-cancellation exceptions.

Two new independent CmdStan fixtures exercise every solver family with scalar
and nested-array integer arguments, including branch-local mutations. One
keeps the callback compiled, the other deliberately requires callback
interpretation with a dynamic-sized temporary. All 138 recorded values match
bitwise at three points. Engine assertions require the output block to stay
compiled in both cases and all solver input adjoint masks to stay zero. The
existing runtime-integer ODE fixture now requires compiled execution too.

Baseline binaries, build/test logs, recorded-oracle logs, and six alternating
performance pairs are retained in `/tmp/stanli-runtime-integer-args`. Timings
include preparation, first/warm gradient, first/warm output row, complete
inference, and process peak RSS. RSS is not retained model memory. The
comparison canaries include an unchanged density callback. Numeric checksums
must agree before a benchmark summary is accepted.

## Native performance medians (six pairs)

| Model / version | Preparation µs | First row µs | Warm row µs | Inference ms |
| --- | ---: | ---: | ---: | ---: |
| gq_callback_runtime_integer_before | 164.0 | 50.0 | 20.09 | 2.329 |
| gq_callback_runtime_integer_after | 139.5 | 18.8 | 1.35 | 0.388 |
| gq_callback_integer_contexts_before | 790.6 | 245.4 | 179.78 | 18.937 |
| gq_callback_integer_contexts_after | 795.1 | 108.1 | 21.46 | 2.448 |
| ode_branch_returns_before | 319.1 | 17.2 | 1.79 | 3.384 |
| ode_branch_returns_after | 315.0 | 13.5 | 1.81 | 3.275 |

Raw samples, dispersion, gradient/source timings, RSS, and binary size are
in `2026-09-28-runtime-integer-callback-performance.json`.
