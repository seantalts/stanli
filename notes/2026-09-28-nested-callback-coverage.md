# Nested callback arrays in the existing engines

This change preserves every array and leaf dimension across retained callback
boundaries. It admits arrays of reals, integers, vectors, row vectors, and
matrices to the existing register compiler. Graph and register values use the
existing graph layout; constant integer arrays retain Stan serialized order.
The shared container adapters convert real constants on entry and interpreter
arguments on fallback. Plain matrix geometry remains unchanged.

No new execution engine or native-code generator is involved. Unknown shapes
and unsupported callback bodies still refuse locally. Runtime integer values,
solver controls, and dynamic allocation are separate gaps.

## Evidence

The pre-change binaries at `/tmp/stanli-callback-arrays` could not evaluate the
new nested ODE or cross-solver model: callbacks fell back, then failed on an
index whose dimensions had been lost. This is a coverage correction, so there
is no valid before/after speedup for those models.

Four fixtures have independent CmdStan 2.40 recordings at three points. The
nested ODE and its deliberately dynamic-sized fallback version match every
recorded value bitwise. The cross-solver fixtures exercise ODE, DAE, algebra,
quadrature, and adjoint ODE callbacks in ordinary graph execution, runtime
branches, and generated quantities. Their outputs are bitwise equal to Stan;
the density differs by at most 1 ULP.

Their combined rate gradient differs by 14–16 ULP. A separate diagnostic
selects one solver at a time. At point zero, Stan and Stanli gradients are:

| Solver | Stan | Stanli |
| --- | ---: | ---: |
| RK45 | 0.59999999999999998 | 0.60000000000000009 |
| DAE | 0.59999999999999998 | 0.60000000000000009 |
| Algebra | 6 | 6 |
| Quadrature | 0.12 | 0.12 |
| Adjoint ODE | 0.59999999999998632 | 0.59999999999999987 |

The callback is constant in state and time, with slope derivative 3; two
length-0.1 integrations have analytic derivative 0.6. This attributes the
larger discrepancy to existing adjoint ODE sensitivity integration, not an
array permutation or compiled callback derivative. Disabling direct RK makes
no difference. The intentionally interpreted callback agrees with compiled
execution. The new reference tests permit 16 ULP only for this one gradient
column in the two cross-solver fixtures and additionally require agreement
with the complete analytic gradient within 2 ULP. Every other column retains
10 ULP. This is a measured exception, not a universal 10-ULP claim.

Unit tests compare compiled and interpreted weighted gradients bitwise,
including individual array elements. Geometry tests cover equal flattened
lengths with different shapes, zero inner/leaf extents, missing metadata,
negative extents, and size mismatches. Cross-path tests also force the entire
output block through interpretation, which caught a third shape transfer in
`higher_order_eval.cpp`; that transfer is now fixed.

A clean Release rebuild was used for the RhsArg layout change. Full CTest ran
299 cases; its one cross-path failure was fixed and all seven affected checks
then passed. The complete recorded corpus passed: 329 models, 1,020,194 values,
124 same-platform ULP gates. Existing corpus maximum scaled error is 9.38e-13
(7040 ULP near cancellation), unchanged.

Raw logs, the per-solver diagnostic, and six paired performance samples live
in `/tmp/stanli-callback-arrays`. Performance compares the corrected compiled
nested ODE to its mathematically equivalent dynamic-sized callback fallback,
and compares an unchanged ordinary callback before/after this patch. The
fallback fixture contains additional scratch allocation to require
interpretation; it is not the same MIR with an engine switch. Checksums agree.
Process peak RSS is measured, not retained model memory.

## Native performance medians (six samples)

| Model / engine | Preparation µs | Warm gradient µs | Inference ms | Peak RSS MB |
| --- | ---: | ---: | ---: | ---: |
| ode_nested_callback_after | 499.2 | 2.656 | 4.814 | 28.88 |
| ode_nested_callback_fallback_after | 502.4 | 427.027 | 591.087 | 29.34 |
| ode_branch_returns_before | 323.9 | 1.888 | 3.342 | 28.17 |
| ode_branch_returns_after | 317.4 | 1.878 | 3.178 | 28.29 |

Source, first-gradient, output-row, dispersion, binary size, and raw samples
are retained in `2026-09-28-nested-callback-performance.json`.
