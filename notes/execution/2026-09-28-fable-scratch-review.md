# Fable review: Inactive solver scratch

Verdict: the kernel-side change is internally consistent and safe. The stated invariant "all write_array solver activity masks are zero" has one concrete hole on the lowering side, and the graph-path half of that invariant is verified only empirically. Neither blocks the kernel change, but the hole should be closed in the same PR.

**Verified in the four kernel files**

- **Activity decoding is identical across forward, backward, and scratch sizing in every file.** ODE uses bit 0x10 for the four-bit nibble, else bit 0x4 for the two-bit field, else the legacy default 0x3. DAE uses bit 0x8 for three bits, else 0x7. Algebra uses bit 0. Quadrature uses the low three bits. No decode drift between entry points.
- **Mask zero returns zero scratch before any slot length is read.** That is why the million-element sizing check passes, and it is the right proof: the size is independent of input geometry by construction.
- **Mask-zero forwards never touch scratch.** ODE takes the constexpr all-false branch, DAE takes the data solve, algebra takes the double solve, quadrature takes the all-false instantiation with a constexpr guard. The direct RK path, which fills scratch unconditionally, is reachable only from the active else-branch.
- **Mask-zero backwards return before any adjoint or scratch access** in all four kernels. Adjoint ODE was not re-read and I take the "already guarded" claim as stated.
- **Legacy decoding is preserved.** A bare ODE variant still yields 0x3 and a bare DAE variant still yields 0x7, so old recorded graphs allocate exactly as before.
- **values_only paths are unchanged** in ODE, DAE, and algebra. Quadrature has no values_only check and relies on the mask alone, which is consistent.

**Lowering-side invariant, Program/CALL path**

Five of six emission sites force the mask to zero in write_array and OR in the explicit-inactive bit, so the kernels' legacy default is never reached: variadic algebra at `runtime/src/lower_higher_order.cpp:120`, quadrature at lines 182-187, legacy and modern ODE at lines 279-294 stamping 0x4 and 0x10, adjoint at lines 364-372, DAE at lines 421-428.

**The one actual gap.** The legacy `algebra_solver` and `algebra_solver_newton` Program path at `runtime/src/lower_higher_order.cpp:505` computes activity from `data_only` alone with no write_array guard. In a generated-quantities block where the parameter vector is derived from parameters, that emits variant 0x1, so the kernel allocates an output-by-parameter Jacobian and takes the var solve unless values_only happens to be set. Impact is performance only, since the kernels are correct for any variant, and it is not the large-runtime-matrix case because the legacy signature keeps real data separate. The fix is the one-line guard already used at line 120.

**Not verified.** Lines 1100-1388 of the lowering file hold the graph-side ODE, DAE, adjoint, and the rest of legacy algebra lowering, including its variant line. I ran out of reads before them. The new test iterates the write_array graph ops and asserts the explicit-inactive variants, so the graph-path invariant holds empirically for this fixture, not by proof. The quadrature graph path at lines 1034-1036 guards theta but not the bounds' autodiff flags. That can only cost a scratch of two plus the parameter count for one output, so it is a consistency asymmetry rather than a concern.

**Test coverage.** The claim that the test checks both ODE encodings is not enforced. The assertion accepts variant 0x4 or 0x10 with an OR, and the count of two ODE ops distinguishes ordinary from register calls, not legacy from modern. If the fixture has two modern ODEs, the legacy explicit-0x4 path is untested. Counting two-input and four-input ODE ops separately would tighten it. The `!kernel->scratch_size ||` clause lets the sizing check pass vacuously if a kernel drops its scratch registration. All five register one today, so that is latent, not current.

## Dispositions

The legacy algebra Program emission now also applies the write_array activity
guard and zero input-adjoint mask. Its runtime x_r support is still outside
this packing extension. The shared-function fixture calls an ODE and a legacy
algebra solver from both model and generated quantities. Graph emit_ode and
legacy algebra derive their scalar flags from Val::autodiff; that flag is false
in write_array. The new test now counts two-input and four-input ODE encodings
separately, asserts scratch-size registration except for the intentionally
scratch-free adjoint ODE, and verifies absent mask markers retain legacy active
scratch. Finite, infinite and NaN seeds leave all inactive adjoints untouched.
