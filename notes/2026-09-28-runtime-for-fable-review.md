# Fable review: runtime for-loop coverage

Reviewed through the Claude CLI. These are source reviews, not claims that the
reviewer ran the tests. The implementation review was resumed with tools disabled
after a lengthy investigation, to request a bounded conclusion. The final
upper-bound guard was supplied as a description in that concluding prompt;
the lead inspected its implementation and ran the tests.

## Dispositions

- The plan review's reachability blocker was incorrect. The checked-in ODE
  fixture derives a local integer bound from real state. It demonstrably moves
  from MIR interpretation to register execution without changing callback
  integer formals, standalone cache keys, or generated-quantities routing.
- Per-trip initialization, bound copying and exit patching are implemented.
  Integer-array writes and unsized adoption remain conservative refusals;
  while-nested runtime for loops also remain refused.
- Pinned stanc-generated C++ confirmed the upper-bound re-read difference.
  Admission now requires an invariant, effect-free upper bound. Tests explicitly
  refuse body mutation and effectful upper calls. The existing fallback's
  differing behavior is documented separately; no universal equivalence claim
  is made for that unsupported case.
- The suggested no-loop integer-overflow fixture now asserts refusal containing
  `integer arithmetic`. The new source admission checks the complete enclosing
  function and callees. The review's raw Decl-initializer path predates this
  patch; the added source fixture confirms that this change does not newly admit
  the proposed no-loop counterexample. No broader runtime-integer support is
  claimed.
- Refused loop fixtures now assert the specific reason: dynamic shape,
  integer arithmetic, integer-array writes, nesting, and mutable upper bounds.
- Added a previous-iteration real-value fixture to test compaction across a
  back edge. It matches interpreted values and weighted gradients bitwise.
- Added a runtime counter indexing callback integer data. It compiles and
  matches the interpreter; the test now establishes this narrower capability.
- The current while-nested fixture uses a real outer counter and asserts the
  nesting reason, so an incidental integer-arithmetic refusal cannot hide it.
- Islands without a checked function scope remain excluded. The change is
  callback/function coverage, not top-level generated-quantities loop coverage.
- The review's “no new cliff” wording is broader than the timing evidence.
  The results note retains measured preparation costs and canary uncertainty.

All added reviewer fixtures pass in `test_ode_prog`. The unchanged production
candidate also passed 293 CTests, the full recorded CmdStan corpus, installed
Python/R tests and the public-API sampling comparison before those test-only
additions.

## Plan review (verbatim)

**Verdict:** the mir_prog design is sound in outline, but as scoped it is unreachable from every production caller except one narrow case, so the matched phase benchmark would measure nothing. Keep the mir_prog scope, pair it with one small gate change in lowering, and fix two latent per-trip initialization gaps the `while` machinery already carries before building the `for` on top of it. I could not run anything, so the two suspected latent bugs below are readings, each with a fixture to confirm.

## Blockers

- **No caller hands ProgramCompiler a runtime `for` bound today.** Generated quantities rethrow on unevaluable bounds at `runtime/src/lower_stmt.cpp:1502`, which truncates the whole section to WaInterp before any island is compiled. The log_prob path routes runtime bounds to `try_lower_region`, and the structured-loop engine already has a runtime-bound `For` node at `runtime/src/lower_structured_loop.inc:1500`. The standalone function API binds every integer argument as a compile-time constant at `runtime/src/function.cpp:287` and keys its cache on integer values, so bounds there always unroll. ODE and higher-order callbacks require constant ints. The only reachable case is a `for` nested in or after a structured `while` whose bound reads a reified loop-carried int or a dynamic index read. That currently bails the whole island, so the patch is a correctness-neutral improvement there, but it has no benchmark signal.
- **Preferred fix for reachability:** in the generated-quantities branch at `lower_stmt.cpp:1501`, route to `lower_runtime_ifelse(s)` when a bound satisfies `runtime_int_value`. The island's `bind_extern` already imports a runtime UInt graph slot as a register live-in, so the mir_prog side needs nothing extra. Attach the phase benchmark to that gate, not to the mir_prog change. GQ has no backward pass, so it is also the cheapest measurable win.
- **Drop the standalone "changing bounds across calls" test.** It cannot reach the runtime path without changing the int binding and cache key in `compile_function`, and `check_integer_contract` refuses integer arithmetic there anyway. That is a separate scope decision.

## Semantic pitfalls in the while machinery you would inherit

1. **Prologue-hoisted initializers run once per program, not once per trip.** `reify_written_int` at `runtime/include/stanli/mir_prog.hpp:197` and the `late_bound` NaN fills are prepended by `finish()` at line 3516. An SInt declared inside the body folds at line 3596, and a later conditional write reifies it with the declared value in the prologue. Trip two then starts from trip one's final value. Fixture: `for (i in 1:n) { int k = 0; if (i > 1) k = k + 1; out[i] = k; }` should give `0,1,1,1`; my reading predicts `0,1,2,3`. The same shape applies to `while` today. Fix: at runtime loop depth above zero, SInt declarations always take a register with the constant emitted at the declaration point, and unsized adoption fills at the declaration rather than in the prologue.
2. **Int-array folds leak across trips.** `expr(Var)` reads registers first at line 1444, so per-trip arithmetic on a declared int array is fine. The gap is `cint`, which consults `known_int_arrays` at line 754, and the array assignment path updates that table at compile time when `fold_is_certain` at line 3901. Inside a runtime loop a later `y[pos[1]]`, an inner bound `1:pos[1]`, or a declared extent folds to trip one's value. `reify_written_int` silently no-ops for anything not in `ints`, so the `while` pre-pass does not protect arrays. Fix: in the pre-pass, erase `known_int_arrays` and `known_int_array_dims` for every written name, or bail.
3. **The pre-pass itself is mandatory, not optional.** The `while` case reifies every assigned int before compiling the body at line 4047. Without it, a read of `k` before `k = k + 1` compiles to a constant on the first trip and the reification comes too late. Reuse it verbatim, plus the array erasure above.
4. **Bound capture must copy.** `expr(Var k)` returns k's live binding register at line 1444, not a snapshot. If the body writes `k`, "captured once" is false unless both bounds are moved into private registers before the head.
5. **Return inside the loop must be a runtime exit.** The `Return` case decides via `structured_while_depth > return_while_base` at line 3975. If the runtime `for` does not bump that counter, a top-level `return` in the body throws `Returned`, which the `For` handler does not catch, and the function's value is decided at compile time for a loop that may run zero trips. Bump the same counter and set `structured_while_seen`, so the scalar assignment gate at line 3706 also reifies.
6. **Break and continue.** Mark the frame `structured` so top-level `break`/`continue` emit jumps rather than throwing `CompileBreak`, which the unrolled handler treats as "stop unrolling". `continue` must target the increment block, not the head. If the body returns on every path but has pending continues, still emit the increment and back edge; the `while` case's `if (!body_returned)` at line 4067 is not sufficient for a `for`.
7. **`cint` to `try_cint` widens the runtime set.** Forms `cint` refuses today, such as unknown integer functions at line 899, currently bail the region; after the patch `expr()` may accept them and compile a runtime loop. Probably fine, since UInt division already selects `IDIV` at line 3262, but check which corpus refusals flip.
8. **Index surface with a runtime counter is small.** Reads only through `DYN_INDEX` on a final scalar index into a flat leaf, lines 1623 and 1663. Writes only through `DYN_SET` on vector or flat kinds with one index, line 3928. Matrix `m[i, j]`, a runtime leading array index, slices, `segment`, and any declaration sized by the counter all bail. That is the correct refusal, and it also calibrates expectations: scalar bodies over vectors and 1-D arrays are the practical coverage.
9. **Bound evaluation count.** MirInterp captures both bounds once at `runtime/include/stanli/mir_interp.hpp:546`. The CmdStan fixture with a mutated upper bound is the discriminator. If CmdStan re-evaluates, that is a pre-existing gap in both engines, not a new regression. Keep RNG-in-bound out of the first patch.
10. **INT_MAX one-trip loop.** The register counter is a double, so it terminates. MirInterp uses `long`, so it also terminates. CmdStan's generated code increments an `int` past INT_MAX, which is undefined. Use MirInterp parity as the oracle there, and only read the counter into a real; integer arithmetic on it diverges between `int` and `double` mirrors.

## Preferred minimal design and perf framing

Keep the `For` case as `try_cint` on both bounds, then unroll exactly as today. Otherwise generalize the `While` block into one helper: pre-pass reify plus array erasure; move both bounds into private registers; allocate the counter and bind it in `reals` only, never `ints`; head emits `LE` and `JZ`; push a structured frame and bump the runtime depth; compile the body catching `PathExit`; patch continues to the increment; `ADD` with a pooled constant one; `JMP` head; patch exit and breaks; pop and erase the loop variable. Add the in-loop SInt declaration rule from pitfall one. No new opcodes or hot dispatch checks are needed.

For performance, a back edge disables `compact_island` and constant elision at `lower_stmt.cpp:497` and `DYN_INDEX` is flagged no-adjoint, so log_prob islands with a runtime `for` run under var replay. The honest baseline is MirInterp or an island refusal, never the unrolled island, because constant bounds still unroll and the admission test is unchanged. Drive the bitwise MirInterp comparisons through `ProgramCompiler` directly with a seeded bound register, as `tests/test_mir_program_conformance.cpp:629` already does, and land the two latent-bug fixtures against `while` first so the `for` patch inherits verified machinery.


## Implementation review conclusion (verbatim)

**Verdict:** approve with one guard gap to close. I found no confirmed miscompile in the runtime `for` lowering itself. Bound snapshots, continue targeting the bottom test, break and return patching, the INT_MAX final trip, per-trip integer declarations, and the pre-pass reification are all correct as read. Your added upper-bound purity guard closes the CmdStan re-read divergence I had flagged as item 9.

**Confirmed source findings**

- **Integer-contract guard has a bypass outside loops.** The guard fires at loop entry and in the Assignment case only while the name is still in `ints`. A declaration with a runtime initializer takes the pre-existing register path at `runtime/include/stanli/mir_prog.hpp:3684` with no contract check, and every later assignment to that name goes straight to the reals path. So `int x = t > 0.5 ? 3 : 1; x = x + 1; return {x * z[1]};` with no loop reaches `expr()` on an integer add of double registers. Whether `expr()` itself still refuses that form I could not confirm from the diff. Add the guard to the runtime-initializer Decl path, or add a no-loop overflow fixture proving refusal.
- **Islands are excluded by construction.** The guard bails when no return frame exists, so log_prob and generated-quantities regions compiled through the island path refuse every runtime `for`. That matches your "no GQ routing" decision. State it in the note so nobody expects island coverage.
- **Refusal tests check only the boolean.** The ODE case table asserts `want_ok=false` for the while-nested, int-array, shape, and overflow fixtures but not the reason. A fixture that refuses for an incidental reason still passes. Assert the refusal substring, as the conformance harness already does.
- **Redundant but safe.** The Assignment-time int-array bail duplicates the pre-pass bail. The loop-variable `int_decl_at` erase removes a key never inserted. Neither is a bug.

**Speculative concerns, not verified**

- **Register compaction across back edges.** `compact_rhs` runs on looped callbacks. The MOV-aliasing pass treats "single write in program order" as safe, which a back edge can violate when a read sits between the source's rewrite and the copy. The declaration initializer appears to keep such destinations double-written, and the exposure predates this patch through `while`. A fixture with `real x = 0; for (k in lo:hi) { real y = theta[1] * k; value += x; x = y; }` would settle it.
- **ODE data-int arrays indexed by the counter.** Callback integer arrays live in `ints` as a vector, not in `known_int_arrays`. I did not trace `x_i[k]` with a runtime `k` through the index path. It likely bails, but confirm rather than assume.
- **Fixture drift.** The while-nested fixture changed from `int outer` to `real outer` between my reads. Make sure the committed version still refuses for the nesting reason and not because `outer` folded.

**No new cliff introduced.** Constant bounds still unroll, the standalone cache and integer formals are untouched, direct-RK refusal is unchanged from baseline, and the branch-returns canary shows no regression in the saved runs.
