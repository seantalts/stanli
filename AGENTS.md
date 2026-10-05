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

## Read the research index before proposing work

Before considering research ideas, optimizations, or architectural changes,
read [`docs/internal/README.md`](docs/internal/README.md), then only the relevant
topic guide and linked decision/result record. Check what was tried, what
shipped, what was rejected or deferred, and what remains uncertain before
repeating an experiment. Historical plans are evidence, not current task
instructions or authorization; confirm current behavior in code and tests.
Do not load entire archive directories or raw measurement logs into context.
Use the [document catalog](docs/internal/catalog.md) to locate a specific record.

Human-facing guides belong in `docs/`; internal topic guides and historical
plans belong in `docs/internal/`; new research results belong in a topical
subdirectory of `notes/`. Keep one short current summary per topic and link
to detailed evidence. When compacting, preserve decisions, proof obligations,
revision/provenance, negative results and unresolved limits; link the full Git
version for removed detail. Do not rewrite raw evidence. Update the index and
catalog when adding or moving documents. Executable prototypes do not belong
in documentation directories.

## Validation and CI

### Compiler diagnostics

Fix compiler errors and warnings encountered in the affected builds, including
pre-existing diagnostics. Before proposing or merging a code change, rebuild
the affected targets and inspect the complete compiler output. Force those
targets to recompile when cached objects would hide diagnostics, and repeat
the build after fixes to verify that the errors and warnings are gone. Do not
silence warnings, lower warning levels, or ignore a failing compiler to obtain
a green result. Report any diagnostic that cannot be resolved as unfinished
validation; do not describe that build as clean.

### Compiler-pipeline performance

Any change to the compiler pipeline requires a complete corpus benchmark
before merge against the current remote-default-branch baseline. This includes
source-to-MIR compilation, MIR decoding/lowering, graph passes, preparation,
producer/backend selection, compiler pins, and pass flags or budgets. Follow
[`docs/benchmark-protocol.md`](docs/benchmark-protocol.md#compiler-pipeline-regression-checks):
run the full shared application-model corpus for both revisions with the
shipped configuration, matched hardware/toolchains, and retained raw results.
A focused benchmark or a passing numerical corpus replay does not substitute
for the full performance run.

A **2% or greater slowdown** is a major regression. Compare each model's
source compilation, preparation, warm gradients, and fixed-work estimate;
aggregate gains must not hide an individual regression. Investigate apparent
regressions with fresh, controlled measurements and fix confirmed regressions
before merge. Preserve failures, timeouts, and noisy or incomplete evidence as
unresolved; do not drop models, change the threshold, or silently rebaseline
to make the result pass. Include baseline/candidate revisions, commands,
coverage, raw-result links, and the comparison in the PR.

### Required checks and follow-up

Use focused regression tests for the behavior being changed. PR CI retains
Linux and Windows x86_64 native builds, CTest, one complete recorded CmdStan
corpus replay for the shipped configuration, installed Python/R interface
checks, native/JavaScript compiler parity, and inexpensive static checks.
Keep the required status present for documentation-only changes and fail it
if a required source-change check fails or is unexpectedly skipped.

Except for the compiler-pipeline benchmark requirement above, broad
optimization-on/off sweeps, timing comparisons, extra platform and
R-version matrices, sanitizer builds, live upstream compatibility comparisons,
and corpus regeneration belong after merge or on demand. They provide useful
diagnosis and portability coverage without duplicating the PR numerical
oracle. Run the relevant focused or post-submit check before landing when a
change specifically needs that evidence; do not add every broad sweep to
every PR. Preserve release validation dependencies.

[`TESTING.md`](TESTING.md) documents the actual numerical gates, exceptions,
and CI coverage; [`docs/benchmark-protocol.md`](docs/benchmark-protocol.md)
defines performance measurements. Distinguish these measured contracts from
project goals, and report the limits of the evidence.

After merging a PR, arrange a follow-up **about one hour after the merge** to
check CI on main. Record the merge SHA and inspect all workflows for that
commit, plus the latest main commit if it has advanced. Use an available
scheduled follow-up rather than keeping an interactive session waiting. If
scheduling is unavailable, state that limitation and the time a manual check
is due; never imply that a check has been scheduled when it has not. If jobs
are still running, check again until they settle. Investigate failures and fix
them through the normal validated PR process, or report a concrete blocker.
Report the final result with links to the affected runs, then retire the
completed follow-up.

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
