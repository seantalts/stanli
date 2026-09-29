# Stan loop semantics, using the existing engines

Stan is the authority. Pinned stanc emits `for (int i = lower; i <= upper; ++i)`:
the lower bound runs once; the upper bound runs at every condition test,
including the final failed test. Stanli's interpreter and captured-bound loop
paths previously disagreed when an upper bound changed or had effects.

The interpreter now reevaluates the upper expression. The register compiler
uses its existing comparisons and jumps for changing/effectful bounds, while
proven invariant bounds retain the counted-loop path. Graph and structured
lowering only capture an invariant, effect-free upper bound. Other loops use
the register engine, including loops inside a structured candidate that must
transactionally decline. Effectful lower bounds also use register execution.
Fixed container geometry is independent of element mutations, so ordinary
`for (i in 1:size(a)) a[i] = ...` loops retain their previous paths.

Generated quantities may now bind integers computed from earlier runtime
values and compile runtime loop bounds. The change preserves constant folding
where it succeeds. Shape expressions still require a separate geometry proof.
An otherwise empty runtime region with no observable outputs is still refused
by the existing island contract; this patch does not change that contract.

## Evidence

`for_bound_semantics.stan` was recorded independently with pinned CmdStan 2.40.
All 33 density, gradient and output values at three points match exactly
(0 ULP). Cases include shrinking/growing bounds, continue, mutation in else and
nested while bodies, post-loop indexing and loop bounds, callback execution,
and ordered RNG lower/upper calls including an empty loop. Raw MIR tests also
check the exact printed upper-bound evaluation sequence. Callback tests reuse
a compiled program over changing values and compare weighted gradients.

All 294 native CTests and all 329 recorded corpus models pass (1,020,194 values,
124 platform ULP gates). Existing corpus worst differences remain 9.38e-13
scaled error / 7040 ULP; this is not a universal 10-ULP claim.

The new fixture passes with structured loops disabled, preferred, and default,
with compiled callbacks and output paths. Forced structured mode also matches
numerically, but forces an unrelated CSV-emission refusal and interpreted output.
Forced output interpretation matches too, verifying the interpreter correction.

Fable reviewed the plan and initial implementation through Claude CLI; the
[review](2026-09-28-for-bound-semantics-review.md) records dispositions. Subsequent
regressions found by the full tests led to preserving shape-query independence
and using successful static integer evaluation before runtime binding.

## Native cost

Six alternating fresh-process pairs on Darwin arm64, Release/AppleClang 21,
200 ms warmup and 250 ms measurement windows. Before is d7d3591d. Both measured
models already had correct invariant-bound behavior; checksums match. The new
mutable-bound model's old results were wrong, so no speedup is claimed against
that invalid numerical baseline.

| Existing model | Preparation | Native warm gradient | 100 warmup + 100 draws and rows |
| --- | ---: | ---: | ---: |
| ode_runtime_for | 271.9 → 276.0 µs | 5.167 → 5.128 µs | 5.462 → 5.103 ms |
| ode_branch_returns | 314.2 → 319.6 µs | 1.922 → 1.837 µs | 3.266 → 3.397 ms |

These are small fixtures, not a broad throughput guarantee. Whole-process peak
RSS stays around 28 MB; it does not measure retained memory. Raw samples,
medians/MAD, manifests and library identities are in the
[performance artifact](2026-09-28-for-bound-semantics-performance.json).

Reproduce with `ctest --test-dir build-release --output-on-failure -j 4`,
`tools/verify_refs.py PDB --check build-release/stanli_check --jobs 4`, and
`tests/test_scalar_rng_reference.py build-release/stanli_check for_bound_semantics 0`
(using Python 3 for Python tools). Performance uses `tools/bench_model_phases.py`
and `bench_grad --timed --warmup-ms 200 --measure-ms 250` against the artifact's
manifest.

## Remaining authorized work

Continue with native integer operations, nested callback container layouts,
runtime integer/control packing, broader checked selectors and mutation, and
avoiding excessive constant-loop expansion. No new interpreter or machine-code
engine is proposed. Changing-size storage and genuine recursion remain separate
capability questions; interpreter deletion is not the success criterion.
