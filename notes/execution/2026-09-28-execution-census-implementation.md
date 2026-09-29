# Execution census: first implementation slice

Implemented against fetched `origin/HEAD` → `origin/main`,
`5b913866768f351f86eed4d5a7246414530f3b2d`, on
`codex/interpreter-gap-audit`. The base was verified as an ancestor of HEAD;
there were no integration conflicts. This note describes the local, uncommitted
implementation following the [roadmap](2026-09-28-execution-engines-and-roadmap.md)
and [Fable's plan review](2026-09-28-fable-roadmap-review.md). Fable reviewed the
plan, not this implementation.

## Delivered

- `dump_ops model.tmir.sexp data.json --execution-json` emits a versioned JSON
  manifest of the final selected compiled model, including retained regions,
  structured-loop sites, reduction children and solver callbacks. It excludes
  partial write-array graphs when the host will select the interpreter.
- `STANLI_EXECUTION_REPORT=1` emits compilation reports and subsequent C API /
  BridgeStan write-array selection after column discovery. An attached fallback
  and a successfully probed public output path are distinct states. Standalone
  function calls emit interpreter-entry traces, including on errors.
- Scoped, thread-local traces count interpreter construction, preparation
  statement entries and folding/admission probes. Nested scopes cannot hide an
  enclosing strict refusal; violations remain sticky across speculative catches.
  `dump_ops ... --execution-json --forbid-mir` rejects even preparation-time
  interpreter construction. This is deliberately stricter than the existing
  `STANLI_NO_INTERPRETER` fallback policy.
- Legacy `integrate_ode_*` callback refusals now enter the normal fallback
  reports in both graph and register-region lowering. This also makes the
  existing fallback policy catch those paths. Solver arithmetic is unchanged.
- Kernel derivative metadata belongs to the registered implementation.
  Softmax/log-softmax are the first explicitly classified nested-tape kernels.
  Region generated reverse and var replay are reported separately; value-only
  write-array sites say derivatives are unused. Retained program calls whose
  backward pointer differs from the current registration stay unclassified.

There is no added per-instruction tracing. Reporting is opt-in, and the normal
evaluation/gradient loops do not acquire environment lookups. Disabled coarse
entry hooks still have a small unmeasured cost; this patch makes no speedup or
zero-overhead claim.

## What the census established

[Saved JSON examples](data/2026-09-28-execution-census-sample.json) include legacy ODE,
interpreted RNG, a matrix callback, and the ordinary AR(1) model. The artifact
records the source base, final tool hash and each MIR hash. It records selected
structure and coarse preparation observations, not a model-wide runtime profile.

The matrix callback fixture passes a `matrix[2,3]` argument to `ode_rk45` and
reads `[2,3]`. Its register callback is refused with
`right-hand side argument 3 has an unsupported logical view`. The interpreter
adapter then fails during gradient evaluation with
`unsupported index: dims=1 [IndexSingle] [IndexSingle]`. The C API returns a
failure with a negative-infinite log density and zero gradient. This confirms a
current shape-adapter support gap, rather than demonstrating a working fallback
that can simply be ported. The JSON includes the tested unconstrained values and
the analytic value if supported; that value is not an independent CmdStan oracle.

The source audit also needed one retention correction: bounded recursion already
works in the standalone function API, including depth-guard refusal and recovery.
Its compiled replacement must preserve that behavior. Recursion in general model
lowering remains a separate expansion question.

The rejecting RNG fixture exposed a differential-test distinction: a successfully
evaluated partial graph prefix cannot establish success of the complete generated
quantities section. The cross-path harness now emits an explicit incomplete-
comparison note when the full interpreter rejects after such a prefix. Complete
graph outcome comparisons remain strict; no numerical tolerance or ledger changed.

## Validation

- Release native build and **268/268 CTest tests passed**, including the new
  trace, policy, nested payload, selected-path and C API JSON regressions.
- Recorded CmdStan replay: **329/329 models**, all three evaluation points,
  **1,020,194 values**; existing structural/scaled gates and the 124 applicable
  model ULP gates passed. Worst scaled error was `9.38e-13` on
  `gpcm_latent_reg_irt` (7,040 ULP). This is not a universal 10-ULP result.
- Freshly installed Python wheel: `tests/test_python.py` passed in an isolated
  Python 3.11 environment using the newly built runtime.
- Installed R package with installed tests: the suite passed. Its initial two
  skips were rerun successfully with `STANLI_RUN` and `STANLI_STANC` configured:
  native subprocess include paths and the RStan CLI CSV comparison.
- Standalone function tests passed with reporting enabled, producing 34 trace
  records including the recursive `descend` function.
- Diagnostic-on/off regression checks preserve bitwise values/gradients across
  both sides of a parameter-dependent branch and preserve public output columns.
- Repository formatting checks, explicit formatting checks for new C++ files,
  and `git diff --check` passed.

Local logs are in `.cache/execution-census-*`, `.cache/execution-python-*`,
`.cache/execution-r-*` and `.cache/execution-census-functions.jsonl`. Dependencies
use the pinned Stan Math `5252d51d47c1`, Stan `a6806ef8477a` and stanc3
`d58446e631b0`. Cached embedded/compiler artifacts were checked against the repo's
producer-stamp helpers. An initial stock compiler had the same source pin but a
different version label; choosing the matching release-labelled artifact made
both generated-manifest checks pass without editing the tracked manifests.
The cached embedded object targets macOS 26; these tests establish current-host
behavior, not compatibility with the release deployment floor.

No native/JavaScript compiler-parity run, browser runtime test, performance
benchmark or package release was performed. The corresponding CI gates remain
unchanged. No compiler source changed in this slice.

## Remaining census work and next coverage patch

This is the census foundation, not completion of every milestone-0 attribution
goal. Most kernels still report `unclassified`; erased logical shapes are `null`.
Solver derivative choices are marked type/shape/mode dependent. Retained branch
sites do not assert which branch executed. Automatic model reporting observes
compilation and host selection; runtime callback-entry tracing needs an explicit
scope. Construction counts are not execution counts, instruction counts, or
exclusive timing. Traces are thread-local; worker threads need their own scopes.

The next bounded coverage patch remains the eleven missing scalar RNG names,
with independent CmdStan rows and exact stream-continuation/error checks. Keep
the two container RNGs and array forms separate. Preserve the matrix callback
fixture as an explicit support-gap baseline for the later logical-shape adapter
work. Classify more kernel implementations as those paths become optimization
targets; do not infer tape-free execution from opcode names.
