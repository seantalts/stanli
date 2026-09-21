# Project priorities

Stanli should make real Stan models quick to install, load, and run while
remaining faithful to Stan and useful to the surrounding ecosystem. Judge
changes against all of these priorities:

- **Numerical fidelity.** Use CmdStan as the independent model oracle. Aim
  for agreement within **10 ULP most of the time**, including log densities,
  gradients, and per-draw outputs. Preserve rejection behavior, shapes,
  names, and non-finite classifications too. Keep existing tighter checks
  where practical. Measure and explain exceptions, especially cancellation
  and ill-conditioned problems; do not widen tolerances just to make a
  regression pass. A scaled-error gate is not proof of a 10-ULP bound.
- **Speed and memory together.** Measure source compilation, preparation,
  first gradients, warm gradients, complete inference, and peak/retained
  memory separately. Check ordinary small and medium models as well as
  stress cases. An improvement on a large model should not impose extra
  preparation, runtime, or memory costs on normal models.
- **Easy installation.** Favor prebuilt, self-contained packages and simple
  Python, R, and browser entry points. Users should not need a C++ or OCaml
  toolchain just to run a model. Test the installed artifacts, including
  their compiler/runtime discovery and useful error messages.
- **Reasonably small binaries.** Track compressed downloads, installed
  libraries, and browser assets. Avoid unnecessary dependencies, duplicate
  implementations, and template-instantiation growth. Explain size costs
  alongside the capability or performance they buy.
- **Stan ecosystem interoperability.** Preserve Stan language/data/output
  conventions and working C, Python, and R interfaces, including BridgeStan,
  CmdStanR/RStan-style workflows and consumers such as posterior, loo, and
  bayesplot. Compatibility means exercising real clients and artifacts.
- **Interactive model development.** Treat time to a first useful posterior
  as a primary use case, including small and medium models on modest
  machines and in the browser. Warm gradient throughput alone does not
  describe this experience. Present model collections as sources within
  the shared benchmark and numerical corpus; keep collection-specific
  coverage detail in provenance, fixture documentation, and historical reports.
- **Reuse upstream Stan.** Prefer Stan's algorithms, Stan Math, and stanc3
  over independent reimplementations. Keep source pins and artifact
  provenance explicit. Custom kernels or compiler/runtime machinery should
  have a measured purpose and preserve upstream semantics.

Implement optimizations from general semantic proofs, never model names,
source fingerprints, or fixture-specific special cases. Keep a correct
fallback when the proof does not apply. Record exploratory work and deferred
research under `notes/`; production code and executable test fixtures belong
outside that documentation-only directory.

## Validation and CI

Use focused regression tests for the behavior being changed. PR CI retains
one representative native build, CTest, the complete recorded CmdStan
corpus replay for the shipped configuration, installed Python/R interface
checks, native/JavaScript compiler parity, and inexpensive static checks.
Keep the required status present for documentation-only changes and fail it
if a required source-change check fails or is unexpectedly skipped.

Broad optimization-on/off sweeps, timing comparisons, extra platform and
R-version matrices, sanitizer builds, live upstream compatibility comparisons,
and corpus regeneration belong after merge or on demand. They provide useful
diagnosis and portability coverage without duplicating the PR numerical
oracle. Run the relevant focused or post-submit check before landing when a
change specifically needs that evidence; do not add every broad sweep to
every PR. Preserve release validation dependencies.

[`TESTING.md`](TESTING.md) documents the actual numerical gates, exceptions,
and CI coverage; [`docs/benchmarks.md`](docs/benchmarks.md#how-we-measure)
defines performance measurements. Distinguish these measured contracts from
project goals, and report the limits of the evidence.

## Start from current upstream

Before implementation, builds, tests, or delegation, inspect the worktree and
any integration in progress, then run `git fetch origin` and
`git remote set-head origin --auto`. Synchronize the task branch with the
fetched `origin/HEAD`; never assume local `main` or a newly created worktree is
current. Start new task branches/worktrees explicitly from `origin/HEAD`.

For existing work, fast-forward if possible or merge the remote default branch
while preserving task changes. Verify
`git merge-base --is-ancestor origin/HEAD HEAD` before substantive work, and
report the base SHA or any integration problem. Never discard dirty files,
reset away commits, rewrite shared history, or alter another active worktree.
The lead agent owns synchronization in shared worktrees; delegated agents
verify the agreed base rather than racing to merge it.

An explicitly requested historical revision, PR head, or benchmark baseline
must remain at that revision; fetch and report divergence instead. If fetching
or safe integration is blocked, report it rather than silently using stale
code. Fetch again before PR creation/merge, integrate new upstream changes,
and rerun affected checks.
