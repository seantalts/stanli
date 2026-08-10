# Semantic Convergence and Simplification Plan

**Status:** adaptive replacement for the earlier mechanical reduction campaign  
**Behavior baseline:** `d2abba1` (`v0.6.0`)  
**Audit evidence snapshot:** `684d2b46e199df05025608a2d90aaaff43027f63`  
**Experimental worktree:** `.worktrees/layout-schema-spike`  

## Objective

Make the runtime substantially smaller and easier for Stan C++ developers to
understand without changing supported behavior or performance. Code reduction
is evidence, not the arbiter: remove concepts and invalid states first, then
count the lines that disappear.

The original 30–50% reduction target remains a search hypothesis. The current
audits support roughly 250–500 production lines, or about 5–10% of the runtime,
without deleting a subsystem. A larger reduction must come from a demonstrated
architectural collapse or generated vocabulary—not abstraction churn, hidden
feature loss, or moving code into templates/tests.

## Governing invariant

> Engine selection, optimization, or fallback may change representation and
> performance only. It must not change accepted language behavior, logical
> values and dimensions, branch/effect order, derivatives, errors, RNG state,
> output names/order, or solver selection.

Graph, register Program, and MIR interpretation remain separate mechanisms.
They do not need one physical value representation or one universal evaluator.
Every fast path must instead have one of three explicit outcomes:

1. execute with equivalent semantics;
2. refuse before execution and use a semantically complete fallback;
3. reject invalid MIR consistently in every route.

Silent no-ops, approximate compilation, and fallbacks narrower than their fast
paths are correctness defects.

## Equality tiers

| Tier | Contract |
|---|---|
| **E — exact semantics** | Same logical type/shape, selected branch, effect order/count, assignments, errors, RNG stream, output schema, and solver controls. Required everywhere. |
| **B — bit exact** | Same double bits and full autodiff Jacobian bits when routes call the same primitives in the same order. |
| **U(k) — named ULP budget** | Allowed only for an enumerated reduction whose tree intentionally differs. The test names the operation and `k`; there is no general relative tolerance. |
| **ODE — route exact** | Same solver family, defaults, arguments, RHS values/Jacobian, and result shape. |
| **RNG — stream exact** | Same values, draw count/order, and next RNG state for the same seed and call sequence. |

External CmdStan reference tolerances are separate from internal route parity.

## Architectural decisions

### Keep

- Graph, Program, and MIR as distinct representations.
- The current graph hot path and flat arenas.
- Program inheritance where `IslandProg`/`RhsProgram` are genuinely Programs;
  do not rewrite inheritance for style.
- Separate island and ODE workspaces; nested execution makes that separation
  meaningful.
- C++17 as the behavior baseline. Evaluate C++20 in an isolated toolchain
  packet; do not mix a language-mode bump with semantic refactors.

### Introduce narrowly

- **Constrained serialization layout:** declared logical dimensions plus an
  innermost-matrix flag, derived from `decl_type`, owning CSV ordinal→name and
  CSV ordinal→constrained-arena offset. It is not a universal slot layout.
- **Typed program value:** register range plus enough logical shape to refuse
  or implement matrix/N-D semantics honestly.
- **Positional bound argument:** one tagged argument sequence; never split real
  and integer arguments and reconstruct their order.
- **Closed statement effect:** classify reject, print, checks, writes, and
  unsupported effects once. Unknown never means no-op.
- **Typed compile disposition:** compiled, unsupported, invalid, or resource
  limit. Hosts map unsupported to their fallback and invalid to one error.
- **Output schema:** immutable names/chunks/width discovered without evaluating
  generated quantities or advancing RNG.
- **ODE call descriptor:** solver identity, controls, legacy/modern form, and
  positional typed arguments. Compiled and interpreted mechanics remain
  separate.

### Do not introduce

- A universal Graph/Program/MIR value type.
- A generic builtin registry or expression visitor.
- Hot-loop virtual dispatch or polymorphic layout objects.
- A generic `Layout` that is reused for data, ODE output, parameters, and
  arbitrary slots; those have different physical provenance today.
- Compression of tests merely to improve the line count.

## Confirmed correctness blockers

These must be characterized before structural cleanup in the affected code:

1. Program `&&`/`||` evaluates both operands; MIR short-circuits.
2. Program drops every non-returning function call; MIR handles reject/print
   but silently drops unknown effects.
3. Uninitialized real locals are zero in Program and NaN in MIR; partially
   written graph containers have another default path.
4. Program erases shape, accepting matrix indexing/multiplication with flat
   scalar semantics.
5. Program separates integer/real UDF arguments and can permute source order.
6. MIR's generic unary fallback strips autodiff, producing zero ODE fallback
   sensitivities for functions such as `sin`, `lgamma`, and `log1p`.
7. Program empty `sum` reads unwritten state; signed-zero/reduction identity
   also differs.
8. Interpreted write-array rejects rank-greater-than-two parameters and had an
   ad hoc rank-two transpose.
9. Interpreted write-array supports only legacy `integrate_ode_*`; adding an
   unrelated RNG can make a working modern ODE path fail.
10. Interpreted output schema discovery mutates state during evaluation,
    depends on probe values, and consumes RNG.
11. Default Graph copy leaves `Op::idata` pointers referring to the source
    graph's pool.

## Phase 0 — finish or reject the constrained-layout spike

The current spike established red/green evidence:

- compiled and direct interpreted `wanames` rows already agree;
- the C API emitted `v` instead of `v.1`, flattened matrix names, and paired
  array-of-matrix names with the wrong value order;
- one serialization descriptor fixes names and values;
- one arena→logical boundary removes three duplicated host conversions and
  the interpreter's rank limit;
- 35/35 CTests and 129/129 recorded CmdStan references pass, with the same
  worst model/deviation as baseline.

The experimental implementation is not merge-ready until it:

1. splits full constrained declarations from `FnWriteParam` output chunks;
   `ParamView` currently represents both and interpreted chunks fake a slot;
2. names the descriptor `ConstrainedLayout` or `SerializedLayout`;
3. checks negative dimensions, overflow, matrix rank, zero width, and every
   transform's constrained width;
4. keeps an identity fast path and a precomputed permutation for nonidentity
   layouts, with no allocation or division per live row;
5. adds matrix versus 2-D-array, nested array/vector/matrix, structured
   transform, and zero-width tests;
6. records the C API name/order change explicitly as a public correctness fix;
7. proves unchanged graph dumps and acceptable live-row throughput/RSS.

Revert the architectural part if it needs provenance-specific data rules or a
dimension vector on every arbitrary graph slot. That would be a false universal
layout, not the narrow contract this spike is meant to prove.

## Phase 1 — build the semantic conformance harness

Extend `test_ode_prog` or add `test_mir_program_conformance` with one observation
model for value bits, dimensions, full Jacobian, effects, and error category.
For refused Program cases, actually run the MIR fallback.

First four red cases:

1. both short-circuit polarities with an invalid RHS;
2. an uninitialized real local;
3. matrix row indexing (`sum(A[1])`);
4. mixed runtime/constant integer UDF arguments.

Then add empty/signed-zero sum, matrix multiplication, UDF-in-branch, taken and
untaken reject/print, unknown effects, unary fallback derivatives, integer
division, while/early-return refusal, and missing return.

Add corresponding suites for:

- compiled versus direct interpreted write-array, including a late unrelated
  RNG metamorphism;
- compiled versus interpreted ODE RHS values and full Jacobians;
- carved versus uncarved graph under B or an explicit U(k);
- native island adjoint versus replay under its named budget;
- failure category and execution phase.

No production semantic change lands in this phase except a fail-loud firewall
needed to make silent wrongness observable.

## Phase 2 — semantic firewalls and isolated fixes

Land one behavior per commit, in this order:

1. unknown effects fail; unsupported shaped Program operations refuse;
2. short-circuit emission;
3. canonical real/int default initialization;
4. empty/signed-zero reduction policy, checked against CmdStan;
5. positional UDF arguments and function-local branch depth;
6. derivative-preserving MIR unary fallback;
7. integer vocabulary parity;
8. MIR-region fallback for necessity islands;
9. modern ODE parity in interpreted write-array.

Each commit carries a cross-route test that fails on the previous revision.
Do not combine correctness repair with vocabulary consolidation or LOC cleanup.

## Phase 3 — simplify around proven contracts

After Phase 2, pursue these seams in order:

1. Split `ConstrainedView`, `BoundOutputChunk`, and immutable `OutputSchema`.
2. Consolidate compiler declaration metadata around a logical declaration
   descriptor while retaining compact `SlotInfo`.
3. Replace raw graph payload pointers with stable pool indices/handles.
4. Share an immutable MIR function module and closed ODE call identity.
5. Consolidate shaped MIR/DataMap values only after integer, RNG, indexing, and
   autodiff parity tests exist.

For every abstraction, record before/after production LOC, compile time, binary
size, benchmark time, allocations, and the concepts/invalid states removed.
Reject helpers that merely move branches, obscure arithmetic order, or grow the
implementation without closing a correctness gap.

## File-by-file review protocol

Review the pinned snapshot, not a moving branch. Every tracked file under
`runtime/src`, `runtime/include`, and `runtime/kernels`—including the three
runtime prose files—receives exactly one primary review and one overlapping
adversarial review.

Use the strongest available subagents (`gpt-5.6-sol`, `ultra`), organized by
semantic packets rather than arbitrary equal-sized file lists:

1. MIR reader/interpreter/program compiler/effects;
2. lowering, parameter transforms, shapes, write-array;
3. Graph/Executor/ownership/passes;
4. kernels, autodiff, reductions, islands;
5. ODE call/RHS/solver paths;
6. C ABI, tools, and language bindings;
7. tests, corpus oracles, build/toolchain, and runtime documentation.

Each reviewer reports, per file:

- owned contract and callers;
- duplicate policy or invalid state;
- semantic/performance hazards;
- missing orthogonal tests;
- proposed concept deletion and realistic LOC range;
- explicit keep/reject decisions.

An adjudicator checks every cross-packet proposal against both owners. No agent
edits production during audit. File coverage, snapshot SHA, dependency hashes,
and reviewer/adjudicator identity are machine-checked before synthesis.

## Gates

Every production change requires the relevant subset below; a phase completion
requires all of them:

- clean RelWithDebInfo build and warnings unchanged or improved;
- full CTest;
- `verify_refs` with model count and worst-deviation line unchanged unless the
  change intentionally fixes recorded behavior;
- `ab_corpus` for graph-pass changes;
- exact compiled/interpreted conformance observations;
- no-allocation checks for density/ODE/serialization steady state;
- before/after benchmark medians on representative scalar, matrix, island,
  ODE, and large nested-output models;
- graph/op dumps unchanged for metadata-only changes;
- native, Wasm, and public C ABI naming/value parity for output changes;
- production, tests, generated code, and documentation LOC reported
  separately.

## Stop conditions

Pause and re-evaluate when:

- a refactor needs a new numerical tolerance;
- fallback coverage shrinks;
- a public output changes without an independent oracle;
- hot-path allocation/dispatch appears;
- a shared abstraction needs backend/provenance flags at most call sites;
- measured production reduction remains below its added conceptual surface;
- reaching 30–50% would require deleting a supported subsystem.

The campaign succeeds when the runtime has fewer semantic owners and invalid
states, all route differences are explicit capability/refusal decisions, and
the measured code/performance result is better—even if the honest reduction is
closer to 5–10% than 30–50%.
