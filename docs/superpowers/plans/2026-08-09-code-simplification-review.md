# Code Simplification Review Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A principal-engineer-level review of the stanli codebase that finds and lands simplifications: less incidental complexity, fewer concepts, smaller files, with behavior pinned by the project's own oracles.

**Architecture:** Three phases. Phase 1 fans out read-only reviewer subagents, one per code area, each returning ranked simplification candidates. Phase 2 adversarially verifies and ranks the candidates into a work-list with a user checkpoint. Phase 3 applies accepted findings, one theme per git worktree and PR, each gated by the full verification stack.

**Tech Stack:** C++17 (runtime), Python (bindings, tools), JS (npm package), R (binding), CMake, ctest, the corpus oracle rig.

## Global Constraints

Every subagent prompt in every phase includes this section verbatim.

- **Behavior is defined by the oracles, not by tests alone.** A change is behavior-preserving only if: `ctest` passes; `tools/verify_refs.py deps/posteriordb --check build-rel/stanli_check --jobs 8` passes with the worst-deviation line unchanged; and, when a graph pass was touched, `harnesses/ab_corpus.py deps/posteriordb` reports no divergence.
- **Accumulation order is contractual.** Several kernels sum in a deliberate direction to stay bitwise identical to stan-math's var path. Reordering a reduction passes the corpus oracle at 1e-9 and fails the ULP tests. Never reassociate arithmetic in `runtime/kernels/`.
- **Stated design choices are not smells.** Kernel repetition is deliberate ("read one, you can read them all"). The IR is deliberately dull. The X-macro lists in `optable.hpp` exist so opcode, kernel, registration, and lowering cannot drift. Islands refusing propto densities, the six-input `Op` limit, and `jacobian=False` raising are documented refusals, not gaps. A candidate that trades explicitness for indirection is REJECTED-BY-DESIGN.
- **Perf-sensitive paths need numbers.** For changes under `runtime/src/executor.cpp`, `runtime/kernels/`, or the passes: run `tools/bench_grad` on `eight_schools_noncentered`, `radon_pooled`, and `hmm_example` before and after. No regressions above run-to-run noise.
- **Mutation-test guard claims.** Any change touching a safety property (`backward_ignores_input_values`, island refusals, in-place conditions): break the guard on purpose, watch the test fail, restore it. Delete the object file and `touch` the source first.
- **Git and style.** One theme per branch/PR, `gh pr merge --auto --rebase`. If the C ABI, build files, or `tools/exported_symbols.def` are touched, run `gh workflow run wheels.yml --ref <branch>` before merging. PR text: no em-dashes, no "genuinely", no "load-bearing", no AI attribution, Google technical writing style.
- **Build commands.** `cmake -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel -j8 && (cd build-rel && ctest)`.

---

### Task 1: Phase 1 survey - dispatch read-only reviewers

**Files:**
- Create: `docs/superpowers/plans/2026-08-09-simplification-findings.md` (the merged findings list)
- Modify: none (reviewers are read-only)

**Interfaces:**
- Produces: a findings file where each finding has: `id` (area-N), `file:line`, `smell` (one sentence), `proposal` (one sentence), `risk` (mechanical | needs-oracle | design-change), `loc_delta` (estimate), `evidence` (why it is safe).

Dispatch eight reviewer subagents in parallel, one per area. Each gets the Global Constraints, its area brief below, and this instruction:

> Review as a principal software engineer. Prefer deleting code, collapsing near-duplicate concepts, tightening interfaces, and shrinking files over adding abstraction. Read the area completely before proposing anything. Return findings in the schema above, ranked by value. Explicitly list candidates you considered and rejected as REJECTED-BY-DESIGN with the reason. Do not modify any file.

- [ ] **Step 1: Dispatch reviewers A1-A4 (runtime core)**

- **A1 - the compiler.** `runtime/src/lower.cpp` (1,935 lines, the biggest file) and `runtime/src/mir_reader.cpp`. Look for: repeated lowering patterns that could share a helper, dead branches, functions doing two jobs, opportunities to split the file along its existing seams (`lower_expr`/`lower_stmt`/`lower_read_param`/the `lower_*_fn` groups) without changing any signature's meaning.
- **A2 - the graph passes.** `runtime/src/inplace.cpp`, `constfold.cpp`, `reroll.cpp` (860 lines), `island.cpp`. Look for: duplicated graph-traversal and safety-check code across passes, invariants checked in more than one place, `reroll.cpp` internal structure (lane classification vs rewriting).
- **A3 - kernels and optable.** `runtime/kernels/*`, `runtime/include/stanli/optable.hpp`, `legacy.hpp`. Look for: hand-written kernels that could move into the existing X-macro machinery without touching accumulation order; shape-plumbing duplication beyond what `tail_m`/`tail_v`/`tail_scatter_*` already share. Kernel-body repetition itself is REJECTED-BY-DESIGN.
- **A4 - interpreter and register machine.** `mir_interp.hpp` (1,284 lines), `wa_interp.cpp`, `program.hpp`, `mir_prog.hpp`, `ode_prog.cpp`. Look for: duplication among the three register-machine front ends, `eval_fun` organization, whether `wa_interp` and the lowering-time interpreter share what they should.

- [ ] **Step 2: Dispatch reviewers A5-A8 (surface and support)**

- **A5 - executor, sampler, C ABI.** `executor.cpp`, `nuts.cpp`, `capi.cpp`, `capi.h`, `model_adapter.hpp`. Look for: error-path consistency, ABI surface that no binding calls, bind-time logic that could be table-driven.
- **A6 - bindings.** `python/stanli/__init__.py`, `js/index.mjs` + `js/worker.js`, `r/R/` + `r/src/bridge.c`. Look for: logic duplicated across the three bindings that belongs behind the C ABI, naming inconsistencies between bindings, dead parameters.
- **A7 - tools and harnesses.** `tools/*.py`, `harnesses/*.py`. Look for: duplicated corpus-walking/CmdStan-driving code across `verify_refs`, `verify_sample`, `verify_lite`, `ab_corpus`, `fn_sweep`; dead tools; inconsistent CLI conventions.
- **A8 - tests.** `tests/`. Look for: coverage gaps against the five silent-wrongness classes in `docs/hacking.md`; redundant tests; fixture hygiene; tests asserting implementation details rather than contracts.

- [ ] **Step 3: Merge findings**

Concatenate the eight reports into
`docs/superpowers/plans/2026-08-09-simplification-findings.md`, dedup
overlapping findings (keep the more specific one), and record each
reviewer's REJECTED-BY-DESIGN list at the bottom.

### Task 2: Phase 2 adjudication - verify, rank, checkpoint

**Files:**
- Modify: `docs/superpowers/plans/2026-08-09-simplification-findings.md`

**Interfaces:**
- Consumes: the findings file from Task 1.
- Produces: the same file with each finding marked `accepted` / `rejected(<reason>)` and accepted findings grouped into named themes (one theme = one Phase 3 branch).

- [ ] **Step 1: Adversarial verification**

For each `needs-oracle` or `design-change` finding, dispatch a skeptic
subagent: "Try to refute this finding: show the duplication is
load-carrying, the dead code is reachable, or the simplification would
change observable behavior. Read the actual code. Default to refuted if
uncertain." Kill refuted findings. `mechanical` findings are verified by
one skeptic per area batch rather than one per finding.

- [ ] **Step 2: Rank and group**

Score surviving findings by (LOC removed + concepts removed) against
risk. Group into themes so each theme's changes share a verification
profile (for example: "pass-traversal helpers" needs ab_corpus;
"tools dedup" needs only the tools' own smoke runs).

- [ ] **Step 3: User checkpoint**

Present the ranked themes to the user with expected LOC delta and risk
per theme. Proceed to Phase 3 only for approved themes.

### Task 3: Phase 3 apply - one subagent per theme, isolated worktrees

**Files:**
- Modify: per theme, as listed in its findings.

**Interfaces:**
- Consumes: approved themes from Task 2.
- Produces: one merged PR per theme.

Each theme runs in its own git worktree
(`git worktree add ../stanrt-<theme> -b simplify/<theme>`) with a fresh
subagent. Every finding inside a theme follows this cycle:

- [ ] **Step 1: Pin current behavior**

If the code being simplified has no test that would fail under a botched
refactor, write one first and watch it pass against the unchanged code.
For oracle-covered code, record the baseline:
`python3 tools/verify_refs.py deps/posteriordb --check build-rel/stanli_check --jobs 8 | tail -3`.

- [ ] **Step 2: Apply the simplification**

Smallest change that lands the finding. No drive-by edits outside the
finding's scope.

- [ ] **Step 3: Run the theme's gate**

Build + ctest always; verify_refs with unchanged worst-deviation line
for runtime changes; ab_corpus for pass changes; bench_grad three-model
spot-check for hot-path changes; mutation test for guard changes.

- [ ] **Step 4: Commit**

One commit per finding, message stating the finding and the evidence
("verify_refs worst deviation unchanged at 2.6e-12").

- [ ] **Step 5: PR and auto-merge**

`gh pr create --fill && gh pr merge --auto --rebase` per theme branch.
Run the Windows workflow first when the theme touched build files or
the C ABI.

### Task 4: Phase 4 final sweep

**Files:**
- Modify: none expected.

- [ ] **Step 1: Fresh-eyes diff review**

After all themes merge, dispatch one reviewer over
`git diff <start>..main` with no other context: hunt for behavior
changes, lost comments that carried constraints, and inconsistencies
between themes.

- [ ] **Step 2: Full oracle run on merged main**

Build, ctest, verify_refs, ab_corpus, and `tools/wasm_check.sh`. Record
the result in the findings file and close it out.

## Self-Review Notes

- Findings cannot be pre-specified, so Phase 3 defines the per-finding
  cycle instead of literal diffs; the no-placeholder rule is satisfied
  by exact commands and gates at every step.
- The reviewer briefs cover every top-level code directory (`runtime`,
  `python`, `js`, `r`, `web` via js, `tools`, `harnesses`, `tests`).
  `deps/` is vendored and out of scope.
- Type-consistency: the findings schema in Task 1 is the one consumed
  in Tasks 2 and 3.
