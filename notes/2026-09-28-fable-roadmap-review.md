# Fable review and roadmap revisions — 2026-09-28

The user requested an independent Fable review through the `claude` CLI.
The first attempt stopped before model execution because authentication had
expired. After sign-in, the review completed successfully with no tool
permission denials.

- Source reviewed: `5b913866768f351f86eed4d5a7246414530f3b2d`, freshly fetched
  `origin/HEAD` and fast-forwarded before delegation. Runtime source is
  unchanged from the notes' original `0bb5b54c` baseline.
- CLI: `2.1.283`; requested alias `fable`, resolved model `claude-fable-5-1`.
  The initialization event and every model response identify that model.
- Effort: high. Tools restricted to Read, Glob and Grep; no builds, tests,
  benchmarks, edits, Git mutations or nested agents were delegated.
- Command: `claude --print --model fable --effort high --safe-mode --restricted
  --strict-mcp-config --no-chrome --no-session-persistence --tools Read,Glob,Grep
  --allowedTools Read,Glob,Grep --permission-mode dontAsk --permission-prompts
  none --output-format stream-json --verbose`, with the saved prompt on stdin.
- Full prompt, input snapshots, raw stream and machine-readable provenance
  remain locally in ignored `.cache/fable-interpreter-roadmap-review/`.

The review below is Fable's final response, preserved verbatim. Its line
references to the two notes refer to the pre-revision inputs, not the revised
line numbers. Runtime source references still refer to the reviewed SHA.
The dispositions after the review are the lead's source-checked response.
The revised plan has not received a second Fable review.

Reviewed input SHA-256 values:

- `notes/2026-09-28-execution-engines-and-roadmap.md`: `5feda3b8f7568f84c32c3307c2d6e87ddf77509dc16bf45d8450dbf4825bdff3`
- `notes/2026-09-28-interpreter-gap-audit.md`: `a5f8c99dcdb7c13bb6b52974c0bef0a6f66c25b3682d48d37047eef94bf2b7f8`
- `.cache/fable-interpreter-roadmap-review/prompt-retry.txt`: `6232ce48262a466006bd8659ab278a1354dbeed78465a240d17f4376c2de6a2c`

Current documents: [roadmap](2026-09-28-execution-engines-and-roadmap.md) and [source inventory](2026-09-28-interpreter-gap-audit.md).

---

# Review: Stanli execution-engine roadmap (notes/2026-09-28-*)

Reviewed against the shared checkout at 5b913866. Everything below is static evidence from source reads; I ran no builds, tests, or benchmarks, so no runtime or performance claim in this review is measured.

## 1. Verdict

**Ready to implement the first slice after specific revisions** (findings 2 and 5 change what the slice must emit and where it must emit it). The milestone structure and dependency order are sound, and the engine/tape distinctions are accurate where I checked them.

**The eventual native/Wasm backend is not approved by this review.** It remains a hypothesis. Two things must be settled before choosing a backend: the browser linking contract (finding 4) and the amortization budget (finding 3). The plan itself says as much at lines 197-202; I am confirming that stance, not adding to it.

## 2. Prioritized findings

No blocker was found for the first slice.

### High 1. The deletion inventory omits lowering-time interpreter uses

The audit's table (audit lines 35-41) and milestone 4 (roadmap line 141) cover transformed data, initialization, functions, write-array and callbacks. But `MirInterp<double>` is also used *inside lowering* as a speculative evaluator:

- `Lowering::try_eval_interpreter` at `runtime/src/lower_expr.cpp:1331-1366` folds selectors and short-circuit conditions through `td.eval` (call sites at 1413, 1570, 1580).
- The structured-loop admission probe at `runtime/src/lower_structured_loop.inc:68` constructs a fresh interpreter with a 4096-statement budget to evaluate data-only user-function calls.
- The `td` hook at `runtime/src/lower_internal.hpp:821` routes retained higher-order calls in transformed data through `evaluate_retained_higher_order`.

Milestone 4's "shape computations" does not obviously cover speculative folding with a step budget over partially lowered environments (`int_env`, `td.env()`). Milestone 6 cannot delete the class while lowering calls it.

**Revision:** add a row "compile-time folding and admission probes" to the audit table and to milestone 4's deliverable. State that the compiled value engine must be callable at lowering time, on a partial environment, with a work budget and refusal semantics equivalent to today's `catch` at `lower_expr.cpp:1367`.

### High 2. The site manifest must be produced after selection, not during lowering

The first slice (roadmap lines 163-169) asks for a compile-time manifest of "selected value engine" and "local derivative mechanism". Several of those choices are made after, or outside, the lowering pass that would naturally emit the manifest:

- `compile_model` lowers the model twice and picks one (`runtime/src/lower.cpp:993-994`, `try_bounded_specialization`).
- Island acceptance is a cost decision after `gen_adjoint` (`runtime/src/island.cpp:770-838`), and `native_adj` is forced false in write-array (`runtime/src/lower_stmt.cpp:523`).
- Write-array interpreter attachment is decided at `lower.cpp:1024-1038`, then the host *probes* it and may drop it (`runtime/src/capi.cpp:136-166`).
- The `STANLI_NO_INTERPRETER` check at `lower.cpp:1094` runs before that host probe.

A manifest emitted mid-lowering can describe a discarded graph or an island the pricer later rejected.

**Revision:** define the manifest as a walk over the final `CompiledModel` (graph ops, `udata` payloads, `write_array`, `transform_inits`) plus the host probe result, emitted at the end of `compile_model` and again by the driver after probing. Static classification then reads `native_adj`, `prog.ok`, `direct_rk_enabled` and similar fields off the selected objects rather than reconstructing decisions.

### High 3. The 20,000-gradient amortization budget contradicts the stated startup priority

Roadmap lines 229-231 amortize codegen compile cost within "the existing 20,000-gradient workload estimate". The benchmark protocol says its public tables recalculate totals for a 2,000-gradient workload (`docs/benchmark-protocol.md:63-65`), and AGENTS.md makes time to first useful posterior a primary use case. A JIT that pays for itself at 20,000 gradients and not at 2,000 fails the user's stated priority on exactly the small and medium models the plan names as required controls.

**Revision:** report break-even at both budgets and make the 2,000-gradient estimate, including preparation and cache-miss compile time, the gate for "no small/medium regression". Select the "two dispatch-heavy structural cases" by a measurable census property (for example share of gradient time inside `OP_ISLAND`/`OP_LOOP` under `STANLI_PROFILE`), not by model name, so the selection is a semantic rule. The 1.2× figure has no stated basis; keep it provisional but say what it was derived from or drop the number until the census exists.

### High 4. The browser half of the codegen experiment has no linking contract, and that is the concrete blocker

The plan says a browser emitter "must call the same kernel contracts with explicit memory/ABI ownership" (line 200) but does not say how a runtime-generated module would reach them. Current facts:

- The browser build is a MODULARIZE'd main module, not a `MAIN_MODULE`, with an explicit export list read from `capi.h` (`CMakeLists.txt:565-596`). Nothing exports kernel entry points or the function table for outside callers.
- The build comment at `CMakeLists.txt:506` states the browser has neither `dlopen` nor a second process.
- The webR artifact is a `SIDE_MODULE` linked into webR's main module with a matched emsdk (`CMakeLists.txt:607-625`). A model module there would have to link against a side module inside someone else's main module, a third regime.
- Runtime `WebAssembly.instantiate` of generated bytes additionally requires the hosting page's CSP to allow it; no CSP handling exists in `js/` today.

**Needed experiment (before any backend choice):** hand-write a tiny Wasm module that imports the main module's memory and one kernel through the table, instantiate it against the shipped `stanli.js` from `tests/test_wasm.cjs`, call it, and measure the compressed size delta of exporting the kernel set. Repeat under webR if feasible. If this cannot be made to work within the size budget, the Wasm end state needs a different shape (for example ahead-of-time per-model modules produced by a hosted compiler), and milestone 5 should say so.

On the native side, macOS arm64 executable memory (`MAP_JIT` plus write-protect toggling) is a known cost, not a blocker. The cheaper first step is the plan's own "developer-only generated C" comparator: if `-O2 -ffp-contract=off` C compiled from one `Program`/`AdjProgram` does not beat the VM, no shipped emitter will. Note `-ffp-contract=off` is a hard requirement for bitwise agreement (`CMakeLists.txt:131`, `TESTING.md:249`); the plan's "same FP grouping" gate should name contraction explicitly.

### Medium 5. Exclusive timing needs in-kernel clocks; use a simpler stopping criterion

`STANLI_PROFILE` is already inclusive per bound step (`runtime/src/executor.cpp:781-797`), and it is off the fast path via one branch per sweep. `OP_ODE` time therefore includes every RHS evaluation (`runtime/kernels/ode.cpp:60-67`), `OP_ISLAND` includes its `CALL` kernels, `OP_LOOP` includes its body. Getting exclusive numbers as the plan proposes (line 168) means clock reads inside `run_program_impl` or `MirRhs`, which execute thousands of times per gradient. That distorts the thing being measured and touches shared kernel code.

**Revision:** keep the slice inclusive-only plus a static child-mechanism list per site. For the one case where double counting matters (solver wrapper versus RHS), measure the RHS alone with a separate `run_rhs<double>` microbenchmark rather than instrumenting the integrator. Executed-path counts are only needed where the choice is dynamic (`OP_LOOP` versions, RHS kernel choice); island replay versus generated is static per program (`native_adj`) and needs no counter.

### Medium 6. Milestone 2 mixes retention-critical items with expansion, and one current behavior is unestablished

The plan correctly separates unsupported-language work from retaining supported behavior. But some milestone 2 items are retention-critical for deletion and the plan does not mark them:

- Early returns under runtime control are supported today *through the interpreter* (`tests/test_ode_prog.cpp:417` expects `f_early` to require interpretation). Deleting the interpreter without compiling this narrows support.
- `supported_rhs_view` at `runtime/src/ode_prog.cpp:33-42` refuses matrices and arrays of vectors, sending them to the interpreter. The interpreter adapter at `ode.cpp:85-107` flattens each argument into `std::vector<T>` before `MirInterp::call`. The audit (line 116-118) already doubts this preserves matrix shapes. So whether matrix-argument callbacks are currently supported at all is unknown.

**Revision:** tag each milestone 2 item as "retention-critical" or "expansion". Add a matrix-argument RHS fixture to the first slice's list so the census records whether that shape works today, is silently wrong, or errors. A refused fixture is not evidence of a working fallback.

### Medium 7. RNG migration gate should bind to the existing oracles now, before the local oracle disappears

I verified the counts: `interpreted_rng_call` handles 29 names (`runtime/src/wa_interp.cpp:316-455`); the graph route covers 12 `ScalarRng` families plus categorical, categorical-logit, multi-normal and dirichlet, 16 names (`runtime/include/stanli/rng_family.hpp:20-42`, `runtime/src/lower_funapp.cpp:28-31`); the 13-name difference matches the audit. Milestone 1's gate "exact RNG stream position" is testable with existing machinery the plan does not cite:

- CmdStan `write_array` rows recorded at seed 1234, chain 0 (`TESTING.md:210-216`).
- Cross-path bitwise comparison of graph versus attached interpreter (`TESTING.md:508-532`, `tests/cross_path.hpp`), with four ledger entries.

Two consequences. First, once milestone 6 removes `WaInterp`, the only RNG oracle left is the recorded CmdStan rows at three points, so new RNG fixtures should have CmdStan rows recorded in the strict mode that retains CmdStan output even when Stanli fails (`TESTING.md:219-221`). Second, the `ScalarRng` enum reserves variant numbers (`rng_family.hpp:30`), so adding eleven families is an encoding change that must not renumber existing variants.

### Low 8. Accuracy nits in the engine table

- Solver fallbacks construct a fresh `MirInterp<T>` on every callback evaluation (`ode.cpp:106`, `runtime/kernels/algebra.cpp:56,104`, `dae.cpp:57`, `quadrature.cpp:57`, `ode_adjoint.cpp:58`). The roadmap says callbacks "can re-enter" the interpreter (line 60) but not that the interpreter is rebuilt per evaluation, which is what the "~30x slower" comment at `lower_higher_order.cpp:1099` is about. The census should count these as per-evaluation constructions.
- The legacy ODE reporting gap is confirmed: `lower_higher_order.cpp:237-238` and `1322-1325` compile without a note, the modern branches at `249-252` and `1262-1264` add one, and `emit_ode` at `1101-1103` only emits a debug diagnostic. Region-created ODE payloads at `264-270` also skip the direct-RK setup that `emit_ode` performs, as the audit says.
- "Not an existing execution backend" (line 61) is true, but the repository has a "native reduce_sum" prototype (`docs/superpowers/plans/2026-09-18-native-reduce-sum-prototype.md`) where "native" means native child graphs on threads. The roadmap should say its use of "native" differs, to avoid a reader assuming prior codegen evidence exists.
- The claim that a refused region compiler in log-prob is a compile error is correct (`lower_stmt.cpp:466-467`). The claim that `STANLI_NO_INTERPRETER` only checks accumulated fallback notes at model compile is correct (`lower.cpp:1094`); it cannot see `InitInterp` (`capi.cpp:314-328`) or `stanli_function_call` (`runtime/src/function.cpp:230-232`).

## 3. Recommended first three slices

**Slice A: post-selection census.** Emit the manifest from the final `CompiledModel` and after the host probe (finding 2). Add the plan's fixtures plus a matrix-argument callback fixture (finding 6). Fix the two legacy ODE note sites. Acceptance: manifest JSON for each fixture names phase, site, selected engine, derivative mechanism and child mechanisms; a test asserts the legacy ODE fixture now appears in `interpreter_fallbacks` and fails under `STANLI_NO_INTERPRETER`; existing cross-path and corpus replay unchanged; a matched-pair timing run with instrumentation off shows no repeatable change on two ordinary models.

**Slice B: eleven scalar RNG names.** Extend the `ScalarRng` tranche without renumbering existing variants; route graph and region lowering through the shared family. Acceptance: for each name, a fixture with a recorded CmdStan `write_array` row at seed 1234/chain 0 matches exactly; the cross-path harness is bitwise against the interpreter with no new ledger entries; the manifest from slice A shows zero interpreter sites for those fixtures.

**Slice C: codegen feasibility pre-experiments.** (i) Native: dump one census-selected `Program` and its `AdjProgram` as C, build with the developer toolchain at `-O2 -ffp-contract=off`, and run it in a `test_adjoint`-style harness. Acceptance: bitwise values and adjoints against the VM at several seeds including NaN and signed-zero inputs; protocol-conformant paired timings; break-even reported at 2,000 and 20,000 gradients including compile time. (ii) Browser: the hand-written Wasm linking probe from finding 4. Acceptance: a passing `test_wasm.cjs` case and the measured export size delta. Either probe failing its acceptance is a valid, informative result that reshapes milestone 5.

## 4. What I checked, what I did not establish, open decisions

**Checked statically:** `STANLI_NO_INTERPRETER` scope; all `MirInterp` construction sites; RNG name counts on both sides; legacy ODE note omission; island pricing and `native_adj` selection; write-array interpreter preference in the C API; `supported_rhs_view`; register cap (`mir_prog.hpp:224`); structured-loop refusals for stateful ops and register fallback (`lower_structured_loop.inc:2180-2185`); profiler placement; browser and webR link configurations; `-ffp-contract=off`; benchmark protocol budgets; TESTING.md gates for ULP, write-array rows and cross-path.

**Not established:** anything requiring execution. I did not verify the 26 scalar-binary names, the probability-kernel tail, `OP_LOOP` runtime replay mechanics, `reduce_sum` defaults, or the historical 119/119 claim. I did not confirm that matrix-argument callbacks work or fail today. No performance number in either note or this review is current.

**Open architectural decisions the plan should keep explicit:** backend form (minimal emitter versus packaged optimizer versus ahead-of-time per-model artifacts for the browser); tier selection rule and its budget; strict no-interpreter semantics for per-call entry points that cannot be checked at model compile time; whether typed-MIR direct lowering is pursued at all, which should stay parked until slice C reports.

---

## Dispositions after source verification

The first slice is ready to implement as narrowed in the revised roadmap.
This is the lead's assessment after the revisions, not a claim that Fable
approved the revised text. Backend choice and performance remain unproven.

1. **Lowering-time interpreter uses — clarified and accepted.** The original
   audit already included data folding and size evaluation; it did not
   separately enumerate the fresh bounded structured-loop probe. Added a
   distinct row, partial-environment/budget/refusal obligations, and preparation
   entry events. Verified `lower_expr.cpp:1331`, `lower_structured_loop.inc:68`
   and `lower_internal.hpp:804`. No runtime support result is inferred.
2. **Manifest after selection — accepted with an additional boundary.** The
   final model and host/binding decisions supply selected-path classifications;
   discarded speculative candidates do not. `compile_model` tries bounded
   specialization and uses ordinary lowering if needed; this is not an
   unconditional claim that it always lowers the same model twice. A separate
   coarse preparation log captures already-executed folding and probes, which
   cannot be recovered by walking the final model. Derivatives in value-only
   GQ are marked unused, not var replay merely because `native_adj` is false.
   Verified model selection, island pricing, `lower_stmt.cpp:523` and
   `capi.cpp:136-166` host discovery.
3. **Startup budget — accepted, without inventing a universal workload.**
   Removed the unmeasured 1.2× threshold. Require observed break-even and
   setup-inclusive estimates at both 2,000 and 20,000 gradients, plus actual
   first-fit/complete-inference evidence where relevant. The shorter estimate
   is a required control; neither estimate defines every user workload. A
   20,000-only win cannot justify default selection for short runs. Selection
   uses structural evidence and measurements; inclusive island time alone is
   not proof of dispatch overhead.
4. **Generated-code integration — accepted as a feasibility obligation.**
   Added separate native and browser kernel-call probes with state, ownership,
   FP-contraction, exception, memory-growth, size and deployment checks, plus
   a distinct webR gate. Source establishes the current export configuration
   and C++ context/payload contracts; it does not establish that every possible
   table/import route is unavailable. Probe the built artifact before deciding
   the required export changes. Hosted compilation stays an alternative, not
   an assumed new service dependency. **Not adopted:** a failed generated
   C/C++ comparator cannot prove that no future emitter will win; improved
   fusion, lowering or storage can change the result. Native executable-memory
   and browser module-policy requirements must be validated for the chosen
   implementation and deployment, not treated as a universal recipe here.
5. **Timing scope — accepted.** First slice uses existing inclusive timing,
   static child mechanisms and coarse entry evidence. Nested clocks and full
   memory attribution are deferred. Parent/child totals cannot be summed; a
   standalone RHS benchmark is diagnostic and does not measure its actual
   exclusive solver contribution. Static derivative choices need no hot
   counters. Strict interpreter diagnostics must remain observable across
   catches that normally turn speculative errors into refusals.
6. **Retention versus expansion — accepted.** Early callback returns are
   demonstrated interpreter-backed support. Matrix/nested callback arguments
   are unresolved until a baseline fixture runs. General recursion is
   model-lowering expansion. Subsequent implementation inspection corrected an
   overbroad statement here: the standalone function API already supports
   bounded recursion (`tests/test_function.cpp:233-246`). That behavior is
   retention-critical for its compiled replacement. Added these distinctions
   and the matrix-callback fixture.
7. **RNG evidence — accepted with an important correction to the review.**
   Keep existing scalar/container variant numbers and capture independent
   CmdStan outputs in strict recording mode. However, `TESTING.md:521-525`
   explicitly says the ordinary cross-path bitwise gate **excludes stochastic
   columns**. It cannot prove the proposed RNG migration even if no ledger
   entry changes. Added dedicated graph/program/interpreter draw and stream
   continuation tests, including repeated/error paths. A successful slice
   removes interpretation from migrated GQ sites; mandatory preparation
   interpretation may remain. This corrects the review's overly broad request
   for zero interpreter sites across the entire fixture.
8. **Engine-table clarifications — accepted.** State the per-callback fresh
   interpreter construction and distinguish model machine-code generation from
   generated adjoints and native worker graphs. Reconfirmed both legacy ODE
   note omissions and the current environment flag's limited scope. No timing
   claim is attached to interpreter construction or to the old slowdown comment.

The recommended first three slices are now: final-selection census and legacy
ODE reporting; eleven scalar RNG names with explicit stochastic tests; and
bounded native/browser code-generation feasibility probes. Array/container
RNG expansion, exhaustive profiling, callback generalization and interpreter
removal remain later work with their own evidence gates.

Validation of these revisions: current source checks above, local Markdown
link-target and whitespace checks, and `git diff --check`. No production code,
model evaluation, build, benchmark, commit or PR was made for this review.
