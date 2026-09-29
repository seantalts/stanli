# Next steps after the direct execution-coverage changes

September 29, 2026. User-requested read-only Fable review and comparison.
Checkout `7749b8bd188fcab3a0534a056bc30be8df7c8851`, PR #410, not merged.
Fetched `origin/HEAD` remained `d13f7fa907eb85617aba85a9e570de0389cdae86`;
ancestry verified, no integration issues. PR CI had passed at review time.
Claude CLI 2.1.284, requested `fable`, resolved `claude-fable-5-1`, high effort,
Read/Grep/Glob only; completed successfully (`is_error=false`). Prompt, stream
and result are retained under `.cache/fable-next-steps-review/`. This review ran
no builds, benchmarks or implementation experiments.

## Recommendation and comparison

**Change the immediate order: measure remaining execution selection before
committing to bounded-storage admission.** Keep this a limited inventory using
existing tools, not a new measurement framework or a requirement to finish every
coverage combination before another small fix.

| Question | Our previous proposal | Fable's recommendation | Disposition |
| --- | --- | --- | --- |
| What next? | Review closed bounded regions after four direct coverage slices. | First inspect remaining callback/output engine selections; let real repeated fallbacks change the order. | Accept the inventory first. Compiled output for 329 models does not establish compiled callbacks or absence of startup costs. |
| How to handle bounded temporary storage? | Keep the temporary and all length-sensitive uses in the existing structured engine; export fixed-size results. | Same direction, after a small forced-region feasibility probe. | Accept as the default next experiment if inventory finds no stronger target; the probe is not production admission or a speed result. |
| When to introduce local MIR calls? | After direct closures and measured opportunity. | Only for a demonstrated costly case direct bounded support cannot close. | Agree. No such real target has been identified in this work; that does not prove none exists. |
| Missing Stan functions/types? | A separate compatibility backlog. | Small Stan Math-backed handlers opportunistically; larger packing/type changes deferred. | Keep a capability queue alongside optimization. A concrete unsupported model can justify a handler without first proving it is a frequent bottleneck. |

Fable's strongest objection to its own order is that the census may find no new
real-model fallbacks and delay a useful bounded-region experiment. We address
that by committing to a next step after one focused pass, instead of turning
inventory into an open-ended project.

## Proposed next work

1. **One execution-selection pass.** Reuse `dump_ops --execution-json`, the
   recorded corpus's established source/data mapping, the execution benchmark
   manifest and targeted callback/refusal fixtures. Record selected engine,
   context, refusal, data size and whether the case is a real model or an
   intentional conformance/refusal test. A blanket run of every fixture needs
   care: some lack standalone data or intentionally fail.
2. **Measure only candidates that could change the priority.** Static reports
   identify selected paths; they do not report call frequency or elapsed time.
   Use existing scoped traces to distinguish interpreter constructions/probes
   during preparation from repeated evaluation, then ordinary uninstrumented
   phase timings for a shortlisted model. One callback fallback does not by
   itself establish that it outweighs an output cost. Prefer a small register
   admission or shared-handler fix when the evidence identifies one.
3. **Otherwise test bounded-region feasibility.** Use an isolated test wrapper
   to force the existing structured engine around the temporary and its scalar
   result. Preserve the wrapper in the tested MIR: O1 may erase a one-trip loop,
   so inspect the MIR or use a deliberate MIR/O0 probe. The actual switch is
   `STANLI_STRUCTURED_LOOPS=force` (plural). Test correctness and actual engine
   selection before timing. A successful probe demonstrates mechanics only;
   selection policy, capacity budget, liveness, error ordering and normal-use
   performance remain design/implementation gates.

Local MIR boundaries, general dynamic values, recursive frames and machine-code
instruction generation remain deferred. Standalone specialization churn and
preparation stay measurable candidates, not automatically urgent projects.
Small compatibility additions should not be blocked on this architecture work.

## Source checks and corrections to the review

- **Confirmed admission gap:** `region_auto_profitable` admits selected loops,
  not ordinary blocks ([lower_structured_loop.inc](../../runtime/src/lower_structured_loop.inc#L1900)).
  Bounded 1D declarations and extent snapshots already exist
  ([same file](../../runtime/src/lower_structured_loop.inc#L1691)). This supports
  the proposed feasibility probe, not a claim that broad reuse is trivial.
- **Important derivative correction:** the exclusions Fable cites at
  `structured_loop.cpp:462` and `:3394` disable an optimization that reuses
  primal storage; they do **not** exclude dynamic-length operations from all
  structured backward execution. Both ordinary history reversal and frozen
  replay apply logical lengths before calling backward kernels
  ([history](../../runtime/src/structured_loop.cpp#L2277),
  [replay](../../runtime/src/structured_loop.cpp#L3092)). CSE and register-segment
  exclusions are real. The first proposed experiment stays value-only to limit
  scope, not because active bounded regions inherently require var replay.
- **Do not overgeneralize refusal safety:** `emit_value` propagates logical
  length and refuses several unsupported combinations
  ([lower_expr.cpp](../../runtime/src/lower_expr.cpp#L1293)); it is not a
  per-opcode audit or proof that every ordinary binding, index or serialization
  path is ready for broadened admission. Preserve the checkpoint's consumer and
  bounds audit. A reported compile refusal is also not by itself proof of
  complete transactional rollback.
- **Region boundaries:** semantic liveness and reused identifiers matter, but
  arbitrary statement slicing need not be the first implementation. In the
  generated O1 MIR for this fixture, the helper computation has its own block
  before the following RNG assignment. Fable's claim that lexical scope here
  necessarily swallows the later binomial call is too broad. First check whether
  an existing enclosing block supplies a closed fixed-result boundary.
- **Probe flag:** the source reads `STANLI_STRUCTURED_LOOPS`, not the singular
  spelling in Fable's response ([lower.cpp](../../runtime/src/lower.cpp#L10)).
  A one-trip source wrapper also needs the optimizer-survival check above.
- **Evidence limits:** no real hot unbounded case was identified, rather than
  proved absent. Callback construction counts are not timings, and the presence
  of var-based register execution does not discharge future dynamic-storage
  lifetime or derivative tests. Missing scalar kernels should not be called
  cheap before checking domain, overload, gradient and numerical requirements.

## Fable's returned review

The following response is retained verbatim as review evidence. Read the source
corrections above before using its implementation details.

**Recommendation.** Do not start bounded local storage yet. Run a zero-code execution-selection sweep first, then, by default, hold the bounded-region design review. The previous Fable review preferred an opaque local-MIR region kernel. That was before the four slices closed all 329 corpus output programs. With no measured real target left, the local-MIR call drops to third, and the ranking now hinges on whether real models still hit interpreted solver callbacks.

## 1. Ranking of the next three items

**Optimization of already-supported workloads**

1. **Execution-selection sweep. No runtime code.** Run `dump_ops --execution-json` and `verify_refs.py --wa-report` over the 329 corpus models, every fixture under `tests/fixtures`, and the bench manifest. Tabulate callback `value_engine`, the `fresh_interpreter_per_call` flag written at `runtime/src/execution_report.cpp:100`, refusal strings, and `write_array` truncation strings. Inspected: the tooling exists at `tools/verify_refs.py:25` and `runtime/src/execution_report.cpp`. The output-coverage report at `notes/execution/data/2026-09-29-output-coverage.txt` records only COMPLETE per model and says nothing about callbacks, so callback status on corpus ODE models such as `lotka_volterra` and `one_comp_mm_elim_abs` is unknown. Impact: this decides item 2. Risk: none. Pre-committed default: zero callback fallbacks on real models means item 2 is the bounded-region design review; hot callback refusals mean register-compiler admission outranks it.

2. **Either register-callback admission or the closed bounded region, decided by item 1.**
   - *Register-callback admission.* Inspected: the callback version of the runtime-sized local is refused in a different engine. `declared()` at `runtime/include/stanli/mir_prog.hpp:1566-1591` takes every declared extent through `cint(d)`, and `tests/fixtures/matrix_callback_contexts_fallback.stan:3-18` exercises that refusal across all five families. Graph-side bounded storage does nothing here. Inference: a fallback callback builds a fresh `MirInterp<T>` per evaluation at `runtime/kernels/ode.cpp:106`, amplified per solve, per gradient, and under autodiff, so one real hit outweighs a per-row output cost. Risk: medium. `Program` has fixed registers and a length-aware capacity would be new instruction semantics. No new backward contract is needed because register callbacks already run with var.
   - *Closed bounded region in output lowering.* Historical: the only removable-cost evidence is the synthetic 6.97 to 16.25 µs per row from the checkpoint. Inspected: the storage proof exists but region selection does not. Details in section 2.

3. **Local-MIR call boundary.** Gated on a real hot case that a bounded proof cannot close. No such case exists today.

**Engineering to add capability, separate bucket.** These are compile refusals that MirInterp cannot rescue, and log density has no interpreter fallback, per `notes/execution/2026-09-29-mir-interpreter-remaining-uses.md:33-37`. `gaussian_dlm_obs_lpdf` refuses at `runtime/src/lower_funapp.cpp:1119-1125` because an op holds six inputs. Extended `wiener_lpdf`, `hypergeometric_2F1`, `inc_beta`, `inv_inc_beta`, `gp_periodic_cov` and the `discrete_range` distribution functions are listed at `docs/coverage.md:110-137`. The scalar ones are cheap kernels that call Stan Math directly; do them opportunistically. The seven-argument packing contract and complex/tuple types are design decisions. All are evidence-poor on workload frequency, so they are blockers for the long-term ambition, not urgent.

**Deferred with reasons.** Standalone integer churn: `runtime/src/function.cpp:386-406` keys plans on integer contents; the historical churn cost is an interface-only 12.9 versus 6.2 µs and no native inference path pays it. Preparation and initialization: the accepted phase tradeoffs break even in 3 to 19 output rows and no startup measurement shows dominance. Dynamic register values and call frames: no measured trigger exists, so I do not propose work.

## 2. Direct handlers, bounded regions, local MIR

**Direct handlers and admission** come first, and I agree with the branch's sequence so far. They closed the corpus. Their limit is that each needs a named target, which is why the sweep precedes further handler work.

**Closed bounded regions.** I agree with the direction and disagree with two things: that StructuredLoop reuse is simple, and that the design review should precede the sweep. Inspected facts:

- `region_auto_profitable` at `runtime/src/lower_structured_loop.inc:1900-1911` roots a region only at a `for` with exact bounds and at least 32 trips, or a `while`, at outer depth 1. The fixture's helper loop at `tests/fixtures/gq_partial_fallback.stan:4` has a runtime upper bound, so `exact_bounds` is absent and it can never auto-root. The only call sites are the if/for/while paths at `runtime/src/lower_stmt.cpp:1530`, `:1572` and `:1663`. A declaration-scoped root is a new admission kind.
- The ordinary-graph refusal is `sized_len` calling `eval_int` at `runtime/src/lower_stmt.cpp:1011` and `runtime/src/lower_expr.cpp:350`. The region Decl path at `lower_structured_loop.inc:1691-1755` already allocates a proved capacity, snapshots the extent, and fills with NaN or `INT_MIN`. `region_range` handles the ternary at `:400-403`. So the storage proof exists; the selector does not.
- A correction that favors the design: `emit_value` at `runtime/src/lower_expr.cpp:1287-1327` threads one runtime extent into ordinary ops and refuses transactionally with "no form over this runtime-length operand" when a kernel lacks a dynamic form. Kernels receive the logical length through `apply_dynamic_length` at `runtime/src/structured_loop.cpp:607` and `:1751`. The failure mode for a non-length-aware consumer is refusal, not a silent read of capacity. What I did not verify is which opcodes, including `sum`, actually pass that seam; the probe below tests it.
- A cost the checkpoint does not mention: ops with `dyn_lengths` are excluded from CSE at `runtime/src/cse.cpp:96`, from islands at `runtime/src/island.cpp:191` and `:1419`, and from generated backward at `runtime/src/structured_loop.cpp:462` and `:3394`. That is acceptable for value-only generated quantities and rules out a log-density region without var replay.

**Local MIR call.** Third. It is the only design that covers constructs no proof can bound, but every current candidate has a bound.

**Strongest objection to my order.** The sweep runs over a corpus that already compiles output and may find nothing, costing a day while the checkpoint already holds measured removable cost. It also cannot see user models outside the corpus. My answer: it is cheap, the default outcome is pre-committed above, and without it the bounded region would be built on one synthetic fixture with no evidence that real generated quantities declare runtime-sized locals.

## 3. Contracts the proposal still lacks, and the smallest evaluator

- **Root and extent by liveness, not lexical scope.** After O1 inlining the helper's local is a block-level declaration in generated quantities. Lexical scope to block end would swallow the later `binomial_rng`, and regions refuse stateful calls per the comment at `tests/test_write_array.cpp:1471-1476`. Liveness by name must survive O1's symbol-id reuse described at `runtime/src/lower_stmt.cpp:936-940` and `:1005-1008`.
- **A profitability criterion distinct from the 32-trip rule.** The natural one is "admit only where ordinary lowering would otherwise truncate the section," attempted as a transactional trial through `fork_region_trial` at `lower_structured_loop.inc:2070`.
- **A capacity budget number.** The fill at `:1743-1747` costs capacity per evaluation. State what happens when `range->hi` is a large data integer.
- **Runtime negative extent.** Compile refuses only `hi < 0` at `:1708`. A proved range with `lo < 0` must raise Stan's error at run time, never clamp.
- **Export.** Fixed shape only, already enforced at `:2277-2280`. Inherited, not new.
- **Derivatives.** Value-only generated quantities. Say so, given the backward exclusions above.

**Smallest evaluator, zero new runtime code.** Add a test-only fixture variant that wraps the helper body in `for (k in 1:1)` and compile it with `STANLI_STRUCTURED_LOOP=force`, parsed at `runtime/src/lower.cpp:16`. Force bypasses the profitability gate at `:2040` and the one-trip unroll short-circuit at `:2044`, so the existing Decl path, ternary capacity, `size()` query, runtime-bounded loop, `sum` over a logical length, and scalar export are all exercised end to end. Inspect the result with `dump_ops`. If it admits, the design review is about selection and budget only. If it refuses for any reason other than root selection, "reuse is simple" is false and the region's cost rises above the checkpoint's estimate. Proposal only; I have not run it.

## 4. What to authorize first, what would change my view, what to defer

Authorize, in order:

1. The execution-selection sweep. Change trigger: any real model with `fresh_interpreter_per_call` true or an interpreted output program moves that refusal class to the top.
2. The forced-mode wrapper probe. Change trigger: a non-root refusal downgrades the bounded region to "compare against a direct handler for the refusing consumer."
3. Only then the bounded-region design review, with the contracts above, or register-callback admission if the sweep says so.

Defer: the local-MIR boundary, dynamic values and frames, standalone churn, preparation and initialization, the seven-argument packing contract until frequency evidence exists, and stencil research as tabled.
