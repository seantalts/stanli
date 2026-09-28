# Fable review: callback matrix geometry plan

Read-only claude --model fable --effort medium with an eight-read budget.

## Initial dispositions

- Range already has rows, cols, ViewKind, full array dimensions and leaf metadata (mir_prog.hpp:49). Matrix locals/indexing are supported. The gap is metadata transport and admission at retained callback entries.
- Keep optional dimensions empty for legacy bindings; reject matrix formals unless complete validated geometry exists. Do not infer matrix dimensions from flattened length.
- Confirm fallback behavior with the newly written non-square active/data matrix fixture before any runtime edit. Runtime implementation has not begun.
- Pinned independent CmdStan values/gradients are the primary model oracle; bitwise typed-interpreter comparisons are the internal consistency oracle. Both are required.
- Matrix scalar indexing may lower entirely to existing direct-RK-whitelisted opcodes. Test with direct RK enabled and disabled, including adaptive-history-sensitive outputs. The whitelist itself stays unchanged.
- Add an equivalent flattened-vector callback as the supported performance baseline, while preserving a shape-correct interpreted comparator for differential testing. Preserve scalar/vector callback canaries because typed invocation also sets declared dimensions.
- Verify plain-matrix storage at all packing sites and keep arrays out of this first slice. DAE/adjoint adapters must be inspected explicitly rather than assumed covered.

## Review text

Review complete. Eight reads used, no advisor or agents.

**Verdict.** The plan's core premise holds on source evidence, but it misnames the admission blocker, understates how many packing sites disagree, and uses the wrong primary oracle. The bounded matrix-only slice is the right scope. Below, "evidence" means I read it; "hypothesis" means inferred and unverified.

## Is matrix geometry really lost?

- **Evidence: never carried.** RhsArg holds only classification, flattened length, and ints at `runtime/include/stanli/ode_prog.hpp:56`. No site that builds one records extents: modern ODE packing, pack_callback_arguments, algebra, legacy compile_rhs, and pack_data_callback all set only len.
- **Evidence: refused before it matters.** Both compilers reject any Matrix leaf and any depth above one via supported_rhs_view in `runtime/src/ode_prog.cpp:33`. The refusal keys on the callback's formal views, not on RhsArg. That means admission needs three changes the plan does not name: supported_rhs_view, stamp_rhs_view, and whatever Range uses to hold rows and columns.
- **Hypothesis: the real blocker is Range.** stamp_rhs_view only sets a ViewKind. If Range carries no matrix extents, ProgramCompiler cannot lower two-index reads on a formal regardless of what RhsArg stores. Check the Range definition and whether compiled bodies already index matrix locals before designing the RhsArg field.
- **Evidence: the fallback invents one dimension.** The positional call overload at `runtime/include/stanli/mir_interp.hpp:145` sets dims to the flattened length for any non-scalar formal. The typed overload at line 187 preserves dims and calls set_declared_dims, which the positional one does not. Switching fallbacks to the typed overload therefore changes behavior for existing scalar and vector callbacks too. That needs a bitwise canary, not just "unchanged legacy calls".
- **Not verified:** whether a matrix formal currently throws in the fallback or silently mis-indexes. The plan says "may not execute correctly". This is the single most important unknown and should be settled first.

## Shared callback ABI scope

The plan says program and quadrature lowering share pack_callback_arguments. True, but their value lambdas differ, and the disagreement is live once matrices are admitted:

- Program path applies graph_order to data reals at `runtime/src/lower_higher_order.cpp:30`.
- Graph quadrature passes require_constant_reals raw at line 959.
- Modern ODE has its own loop and passes data raw at line 1225.
- Value-only evaluation uses storage_order, which passes plain matrices through unchanged and reorders only multi-dimensional arrays at `runtime/src/higher_order_eval.cpp:45`.

For plain matrices this is probably benign, since storage_order leaves them alone and graph_order likely does too. That is a hypothesis; the non-square data matrix test must prove it at all three sites. For arrays it is not benign, which confirms the plan's decision to exclude arrays.

**Safe scope:** add an optional dims field to RhsArg with empty meaning legacy flattened. Make the compiler refuse a matrix formal whose RhsArg has no dims. Every site you do not touch then fails closed instead of guessing. Put the one binding helper in callback.hpp beside pack_callback_arguments and route both compiled binding and fallback Value reconstruction through it. DAE and adjoint ODE were not read; treat them as unconverted until checked.

## Oracle corrections

- **Primary oracle must be CmdStan.** The plan makes "compiled versus shape-preserving typed interpretation" the correctness gate. Both are Stanli code and could share a wrong convention. Pin CmdStan log density and gradient for the test model first, at the 10 ULP contract, and use interpreter-versus-compiled bitwise agreement as a secondary consistency gate.
- **Direct RK can become eligible silently.** make_rhs_adjoint_program is an opcode whitelist. If matrix indexing lowers to MOV and arithmetic, a matrix RHS passes the whitelist with no new proof. Run the experiment with and without direct RK disabled and compare, or gate direct RK off for matrix-view programs until measured. Hypothesis, but cheap to check.
- **Performance baseline.** The plan is right that the historical matrix path cannot be timed. Use the model rewritten with a flattened vector plus explicit index arithmetic as the "what users write today" baseline. Keep lotka_volterra as the scalar/vector canary for preparation and warm gradient.

## Bounded next experiment

No runtime changes. One day, output is a notes file plus pinned references.

1. Write a two-state linear ODE with a 2x3 parameter matrix, a 3-vector data argument, one explicit `B[1,3]` read that differs under row-major flattening, and one nested UDF call. Add a quadrature variant with a non-square data matrix.
2. Run it today with STANLI_DEBUG_ODE set. Record the refusal text, then whether the fallback throws, matches CmdStan, or silently differs. If it silently differs, file a regression test before any feature work.
3. Grep the Range definition and ProgramCompiler matrix indexing to answer the Range extents question.
4. Pin CmdStan log density and gradient for both models at fixed parameters.
5. Time the flattened-vector rewrite as the performance baseline and record lotka_volterra canary numbers.
