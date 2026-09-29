# Nested loops stay in the existing register engine

Eligible runtime `for` loops inside `while` loops no longer force solver
callbacks through `MirInterp`. On the new native fixture, warm gradients improve
from 9.56 ms to 86.3 µs, and a short inference run from 7.88 seconds to 67.8 ms.
The callback uses the existing register evaluator and Stan Math derivatives.
No execution engine, numerical kernel, dependency, or public API was added.

## The bug and the fix

A local such as `int hits = 1` must reset every time execution reaches its
declaration. The compiler could initially treat that integer as a constant,
then discover that an inner loop needed to update it. Its old conversion to
register storage put initialization at function entry. Inside an outer loop,
that loses the per-iteration reset. The compiler therefore refused runtime
`for` loops inside `while` loops.

The compiler now records the lexical position of folded integer definitions in
repeated scopes. If a definition later needs runtime storage, initialization
is emitted at that position. Unused markers disappear before execution, and
jump destinations are adjusted. Keeping a distinct position is necessary:
an inner-loop backedge must return after its initializer, while the outer-loop
backedge must run that initializer again.

Folded assignments update the recorded position. Inlined function bindings save
and restore their caller's positions, so locals in repeated calls reset too.
Ordinary programs needing no markers removed keep the existing inexpensive
finalization path. The existing typed-integer and fixed-storage checks still
apply; unsupported shape/effect cases retain their established fallback.

This removes an eligibility restriction from shared register lowering. The
three existing production engines remain; this change consolidates another
case into the register engine. Large-loop expansion policy is unchanged.

## Native measurements

Fetched upstream `6ce2018b`, task baseline `5b878354`. Release, AppleClang 21,
Darwin arm64. Six alternating fresh-process baseline/candidate pairs per model.
Compilation, preparation, first evaluations, warm evaluations, and short
inference are measured separately through the public C API. Builds and tests
did not overlap timing runs.

The fixture has one state, three outer iterations, and four requested inner
iterations. It includes a skipped inner loop, `continue`, `break`, accumulating
integer locals, and an inlined helper with its own loop. These are general
language features; implementation eligibility does not inspect model names.

| Phase | Before | After |
| --- | ---: | ---: |
| Source compilation, warmed | 1.025 ms | 0.972 ms |
| Preparation from MIR | 0.337 ms | 0.405 ms |
| First gradient | 9.838 ms | 0.133 ms |
| Warm gradient | 9.562 ms | 0.086 ms |
| First output row | 8.801 ms | 0.075 ms |
| Warm output row | 8.609 ms | 0.045 ms |
| 100 warmup + 100 draws, including output rows | 7.878 s | 0.068 s |

Preparation increases by about 68 µs to compile the callback; the first gradient
saves about 9.7 ms. Source-compiler timing variation is not attributed to this
runtime change. Whole-process peak RSS medians are 29.21 MB before and 28.90 MB
after; these are not retained-model-memory measurements.

Canaries cover a small branching ODE, the existing fast expanded-loop ODE,
and a 24-parameter AR(1) graph. Short C-API phase timings showed noisy changes
in both directions, including a preliminary preparation increase. The new
finalization code was tightened to preserve the old path when no markers
need removal, then all measurements and validation were repeated.

Longer native controls use eight A/A pairs and eight A/B pairs per canary,
300 ms warmup and 500 ms measurement. Median candidate/baseline gradient-time
ratios are 1.0013, 1.0006, and 0.9946. Their variation overlaps the identical-
binary controls; no reproducible native-gradient regression was detected.
Six additional identical-library preparation pairs range from 0.936 to 1.092,
with median 1.007. These controls do not establish a universal guarantee for
all models, machines, or individual phases.

The shared library grows 816 bytes uncompressed and 1,730 bytes gzip.
Raw samples, medians, median absolute deviations, source/library hashes,
engine reports, and the manifest are in
[the performance artifact](2026-09-28-nested-loop-performance.json).

## Correctness and coverage

- All 310 CTest cases pass after a clean native build and final revalidation.
- All 329 recorded corpus models pass the existing gates: 1,020,194 values.
  Those include scaled-error gates and 124 same-platform ULP-gated models;
  the corpus is not evidence of a universal 10-ULP bound.
- Live CmdStan 2.40 comparisons at four loop sizes (1, 4, 16, 32) and three
  parameter points give 60 candidate values at **0 ULP**. The old interpreter
  path matches those same references. The default-size reference is committed.
- Execution-report tests require register callbacks in both density and output
  phases. The previous interpreter diagnostic is required in the saved baseline
  comparison and absent in the candidate.
- Seeded 100-draw runs, their gradients, and their output rows match bit for bit
  between baseline and candidate for the target and all three canaries.
- Installed Python and R tests pass with the final library.

Existing interpreter/compiler comparisons retain refusal cases and exercise
loop exits, integer limits, and unsupported shapes. The previous unrelated
24-ULP large branching-ODE discrepancy remains unresolved; no tolerance changed.

Reproduce the permanent regression with `ode_nested_for_reference`,
`callback_geometry`, `test_ode_prog`, and `test_mir_program_conformance` through
CTest. `tools/bench_model_phases.py` and `bench_grad --timed` reproduce the phase
and native-gradient measurements using the artifact's manifest and library
identities. Performance is measured evidence, not an assertion that every
possible model can never become slower.
