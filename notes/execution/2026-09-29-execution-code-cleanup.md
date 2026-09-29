# Execution coverage: pre-merge code and benchmark audit

The audit compares PR #407 with fetched `origin/HEAD` at
`6ce2018b5382b459666ab32fe0df50d4a6d30ba4`. The user asked to retain research
findings but remove experimental or unused implementations before merge.

## Removed code

- Native and Wasm code-generation probes: four source files, both CMake
  targets, and the browser link-option factoring used only by the extra target.
- Structured-callback and loop-adjoint prototypes: three implementation/check
  headers, two local benchmark programs, two experiment drivers, and four
  CMake targets. This removes expanded, mapped, selective, and cached history
  implementations, the segment adapter, and their experimental selectors.
- The unapplied mapped-adjoint patch. Experimental code is not retained as a
  patch or moved into the documentation directory.
- All experimental additions to the existing `bench_ode_ceiling.cpp`; that
  file is restored to upstream exactly.
- The unused `execution_matrix_callback.stan` census fixture. Other added
  fixtures have actual regression-test consumers, including the structured
  fixtures that test the existing loop engine against CmdStan.
- The duplicate `bench_rng_paths.py` timing implementation. The maintained
  phase benchmark already measures RNG output through the same public API.
  An unused API binding in that benchmark was removed too.

The original experiments remain recoverable from Git history. Their result
reports now identify historical revisions and clarify that their commands are
not tools in the current tree. Raw measurements and review notes remain intact.
The execution roadmap now records removal rather than implying the prototypes
are still available.

## Retained code and audit boundaries

The production diff extends the existing graph/register paths, preserves Stan
semantics and callback geometry, removes measured kernel tape overhead, and
adds execution diagnostics. New runtime helpers have live call sites; new
instruction cases are emitted by shared lowering and covered by regression
tests. Diagnostics are intentionally opt-in, with documented CLI/C API users
and tests; they are a supported inspection feature, not a disabled engine.

The audit checked added source files, targets, conditional selectors, helper
references, fixture consumers, and experimental include dependencies. It found
no remaining experimental selector or prototype dependency in runtime, tools,
tests, or CMake. This is an audit of this PR, not a proof that every branch in
the entire pre-existing repository is reachable. Correct exceptional and
unsupported-input fallbacks remain necessary production code.

## Benchmark decision

The application-corpus protocol and published tables need no mechanical
change. They measure setup and warm gradients; new table numbers require a
fresh complete corpus run with its own provenance. Historical results must not
be relabeled as measurements of this branch.

Add a small maintained manifest for the shared phase benchmark: nested loops,
runtime indexing, existing fast constant loops/branches, an ordinary graph,
scalar/vector RNG output, and generated-quantity solver controls. Keep the
standalone function benchmark for the public function API. Document both in
the benchmark protocol, with phase boundaries, overhead, numerical checks,
fresh-process comparisons, and memory limitations. These tools measure the
shipped paths and do not implement alternative execution engines.

The focused fixtures remain outside the public application-corpus inventory.
Timing is an on-demand performance exercise, not a noisy CTest threshold.

## Validation

- Reconfigured and rebuilt the default Release target successfully. Removed
  targets are absent from the regenerated native build graph. No production
  runtime source changed during this cleanup.
- Ran all 310 CTest tests: 308 passed initially; two reference tests detected
  stale source hashes from the preceding whitespace-only finalization. Fresh
  pinned CmdStan recordings at all three points produced exactly the previous
  values and provenance, changing only the source hashes. Both tests then
  passed on rerun. No numerical tolerance changed.
- Ran all eight manifest models through the shared phase benchmark, including
  short inference and outputs; ran all three standalone function cases and
  the skip-inference option. All 12 fresh-process invocations succeeded,
  serially after builds and tests. These are tool smoke checks, not new
  comparative performance claims.
- Repository formatting, generated-document consistency, CI-policy tests,
  Python compilation, and full-branch whitespace checks passed. Reference
  searches found no retained includes, selectors, or build hooks for the
  removed implementations. The original ODE benchmark matches upstream byte
  for byte.

Local logs: `/tmp/stanli-execution-cleanup-build.log`,
`/tmp/stanli-execution-cleanup-ctest.log`,
`/tmp/stanli-execution-cleanup-reference-recheck.log`, and
`/tmp/stanli-execution-cleanup-benchmark-smoke.json`.
