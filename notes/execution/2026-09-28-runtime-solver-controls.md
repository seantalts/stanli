# Runtime solver controls in generated quantities

The existing register and graph kernels now accept current-draw tolerances
and iteration limits for ODEs, DAEs, algebra solvers, and quadrature. Adjoint
ODEs additionally accept their vector tolerances, checkpoint interval,
interpolation choice, and forward/backward solver choices at runtime.

Lowering packs controls into an extra inactive kernel input. The kernel passes
those scalars or vector views to the same Stan Math overload it already used.
No solver is reimplemented. Prepared callback definitions and compiled programs
stay immutable; controls are local to each solve, with no deep copy of the
prepared specification and no new differentiation work.

The extension is for generated quantities. Fixed controls in ordinary graph
calls retain the existing representation. Runtime controls inside a register
region use that region's current bindings, including branch-local changes.
The modern and legacy ODE, algebra, and quadrature entry points are covered.
DAE initial/output times remain preparation constants; runtime shapes and
active-density control inputs are not part of this change.

## Validation

Two new CmdStan 2.40 recordings cover four modern ODE methods, three legacy
ODE methods, DAE, two modern and two legacy algebra solvers, all three
quadrature interfaces, and adjoint ODE controls. At three points they compare
84 values with a maximum difference of 1 ULP. The existing runtime-control
fixture compares another 15 values within 1 ULP and now requires compiled
execution. Engine checks require compiled callbacks and zero solver input
adjoint masks.

The ODE unit test compares both runtime-control input layouts against direct
Stan Math calls. It checks values, exception type, and exception message for
zero relative tolerance, negative absolute tolerance, zero step limits, NaN
tolerance, and an exhausted step limit. Repeated evaluations use one shared
prepared specification with different controls.

The first full suite run identified a test-harness assumption that adjoint
ODE kernels always have four inputs. The harness now supplies the new fifth
control input and asserts the complete input count. A clean Release rebuild
then checks the changed kernel contract across the native suite. The recorded
corpus passed all 329 models, 1,020,194 values, and 124 same-platform ULP gates;
its existing cancellation exceptions remain unchanged.

Baseline binaries, clean-build logs, both full-suite logs, oracle recordings,
and six paired native performance samples are retained in
`/tmp/stanli-runtime-controls`. The baseline is the preceding runtime-integer
callback implementation. Timing checksums must agree before accepting the
comparison. Peak RSS measures the process, not retained model memory.

The final clean-build CTest run passed all 303 tests.

## Native performance medians (six pairs)

| Model / version | Preparation µs | First row µs | Warm row µs | Inference ms |
| --- | ---: | ---: | ---: | ---: |
| gq_callback_runtime_control_before | 156.1 | 50.6 | 18.19 | 2.031 |
| gq_callback_runtime_control_after | 163.6 | 29.9 | 1.35 | 0.391 |
| gq_callback_solver_controls_before | 656.3 | 315.0 | 229.23 | 24.770 |
| gq_callback_solver_controls_after | 602.5 | 136.1 | 26.71 | 2.980 |
| ode_branch_returns_before | 317.2 | 13.7 | 1.76 | 3.188 |
| ode_branch_returns_after | 318.1 | 13.5 | 1.83 | 3.135 |

Raw samples, dispersion, gradient/source timings, RSS, and binary size are
in `2026-09-28-runtime-solver-control-performance.json`.
