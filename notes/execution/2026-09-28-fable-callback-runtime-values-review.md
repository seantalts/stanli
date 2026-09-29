# Fable review: runtime callback values

Review complete. I used the five reads on the plan, callback.hpp, ode_prog.hpp, ode.cpp and quadrature.cpp, and rely on your description for lower_higher_order.cpp.

**Verdict.** The proof is sound and the change is genuinely bounded. In write_array every real scalar is double, so a data-qualified real routed through theta with a zero activity mask reaches the same double solve with the same RHS arithmetic. The register compiler never receives x_r values, so moving a real from the x_r region to the theta region changes only register indices, not operation order. Outputs should therefore be bitwise identical to today's interpreted rows, not merely within 10 ULP, and the test should demand that. I found no definite blocker in the files I read, but four items must be verified before implementation because any one of them would make the change wrong or non-local.

**Must verify before coding (potential blockers)**

1. **Zero mask encoding for the two-input ODE op.** In ode_fwd, an old two-input graph without bit 0x4 or 0x10 set decodes as mask 0x3, meaning y0 and theta both active. "Zero activity bits" must mean low bits zero with the mask-present flag set, or the write_array solve becomes a coupled var solve with different step-controller behavior. Check whether write_array always runs under values_only, which short-circuits before the mask, and whether emit_ode sets the presence flag when all Val::autodiff are false. If both are already true, this is a test obligation only.
2. **Program lowering derives theta activity from parameter_count > 0.** Quadrature uses parameter_count as the theta length in both the kernel and run_rhs. After the change it must stay the count of runtime reals while the variant is forced to zero in write_array. Confirm the same separation for the Program-lowered ODE, DAE and algebra solver variants.
3. **Legacy integrate_ode packing.** ode_prog.hpp exposes a fixed-convention compile_rhs taking n_theta and n_x_r separately. If the legacy family lowers through that entry rather than compile_rhs_args with two RhsArgs, a runtime x_r actual cannot be routed to the theta region without touching the compiler. Then legacy is either excluded from "all five families" or handled explicitly.
4. **Standalone function call cache.** Commit c3d0f929 caches compiled function calls with bounded specialization. If a callback-bearing function is called in both the model block and GQ, the cache key must include the packing policy or phase. Otherwise an x_r-layout spec compiled for log_prob could be reused with theta-layout actuals in write_array, or vice versa.

**Effects that are safe as proposed**

- The interpreter fallback in MirRhs and MirIntegrand splits theta and x_r purely from the RhsArg bindings, so a prog that fails to compile still receives correct positional arguments.
- Data-qualified formals accept doubles at runtime. The register compiler cannot fold on real x_r or theta values, so a branch on a data-qualified real argument stays a runtime branch in both layouts.
- Stan Math checks theta and x_r under the same "ode parameters and data" name, so non-finite runtime data rejects with the same message either way.
- Transformed parameters recomputed in write_array already flow through get_active with autodiff false, so the change only widens that existing path to data_only reals. True constants from transformed data will also move to theta in write_array, which slightly widens the ODE scratch matrix. Measure it, but it should be negligible.
- The unread no-parameter placeholder only matters for tape order under var and is a no-op on the double path.

**Plan text needing a small correction.** The RhsArg comment and RetainedCallback comment describe is_param as "carries autodiff". Update both to "supplied from the theta buffer". Also, the evaluator's demand that "dynamic-local callbacks retain interpretation locally" is only consistent with the bounded scope if it refers to the existing kernel-level MirRhs fallback when prog.ok is false. If it means a new per-statement fallback inside a compiled GQ block, that is new architecture and should be dropped from this plan.

**Bounded test obligations**

- Modern ODE in GQ with runtime real scalar, vector and matrix data-qualified actuals whose values change per draw. Assert graph-backed GQ, compiled RHS, zero variant mask, and bitwise equality with the base-commit interpreted rows plus CmdStan agreement on full output rows.
- Same fixture shape for quadrature, algebra solver, DAE and legacy ODE, or an explicit exclusion for legacy if item 3 forces it.
- Callback whose data-qualified formal drives a branch, with the sign of the runtime actual changing across draws.
- Zero-length runtime vector actual, and a matrix actual to confirm get_active records rows and cols like get_reals does.
- Runtime integer actual and runtime tolerance control in GQ. Both must still refuse compilation and produce output identical to today.
- One function containing a callback called from both the model block and GQ, to exercise item 4. Its log_prob gradient must be bitwise unchanged.
- The existing corpus replay as the log_prob canary, plus the retained whole-block-interpreted fixture for higher_order_eval geometry.
- Write_array timing and peak memory against 52ee7217 on the isolated matrix ODE GQ model.

## Lead dispositions before implementation

- Both modern ODE encodings retain their mask-present bit (0x4 or0x10).
  Phase-zeroing applies only to low activity bits, not the presence flags.
- parameter_count/n_th remain runtime buffer lengths. AD masks are independent.
- Legacy integrate_ode and algebra_solver x_r remain compile-time inputs;
  this slice explicitly covers the five modern variadic solver families.
- The standalone Function cache is object-owned in function.cpp. Model/GQ
  Lowering does not call that API or share its plans: separate Lowering instances
  construct their retained specs. UDF-inlining cross-phase coverage still needs
  a regression fixture, but no cache-key change is required here.
- Local callback fallback means existing prog.ok=false adapters, not partial
  statement interpretation inside a register program.
- Zero-activity ODE/DAE/algebra/quadrature currently allocate/clear unused
  derivative scratch. Moving large runtime real arrays into theta would magnify
  that cost. Include the bounded proof that an explicitly zero-activity kernel
  needs no derivative scratch; adjust zero-activity forwards and backwards
  together, retain all active variants, and test with null scratch and sentinel
  adjoints. Adjoint ODE already has no scratch and returns early at mask zero.
