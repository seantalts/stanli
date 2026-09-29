# Remaining RNG name and context coverage

The historical `bench_rng_paths.py` timing script was consolidated into
`tools/bench_model_phases.py` before merge. Use the maintained
[focused benchmark instructions](../../docs/benchmark-protocol.md#focused-execution-path-benchmarks)
for new measurements; the samples below retain their original provenance.

Current authorization: continue interpreter-gap work, prioritizing native
performance. Native instruction generation, stencil JIT and dispatch-JIT
research are tabled; no further backend comparison is planned. Synced with
origin/HEAD 5b913866768f351f86eed4d5a7246414530f3b2d. Baseline: 84945f1b.

Next bounded slice: single-vector `multi_normal_cholesky_rng` and
`poisson_binomial_rng`, plus `categorical_rng` / `categorical_logit_rng` in
register regions. Share upstream owning-Eigen calls across graph, register
and interpreter routes. Categorical-logit graph lowering currently spells
softmax plus categorical; use the actual upstream logit RNG to preserve its
validation and diagnostics as well as draws.

Eligibility: fixed logical vector arguments, square matching matrix for
multivariate normal, scalar integer or explicitly promoted integer result
for vector-to-integer RNGs. Shape-changing and vectorized array-of-vector
forms remain outside this slice. No reordering, batching or draw elision.

Evaluator before implementation: compare seeded full rows and engine states
bitwise against both interpreter and direct Stan Math, under graph and
runtime while control, changed parameter values, independent chains, invalid
arguments, nonfinites, rejection and recovery. Independently record complete
CmdStan rows, names, log density and gradients at three points with 10-ULP
gates. Compare matched public-C-API preparation, first/warm gradients and
outputs against the saved baseline, with existing RNG and AR1 canaries. Run
full native tests and recorded numerical corpus before integrating.

## Implemented coverage and proof boundary

- Graph and register `OP_RNG` now share single-vector Cholesky-normal and
  Poisson-binomial calls, completing the two remaining RNG **name** gaps
  from the original inventory. This does not imply complete overload coverage.
- Both categorical forms now compile inside register regions. Explicit
  integer-to-real promotion is admitted; draws remain runtime effects.
- Categorical-logit uses its actual upstream RNG, including finite checks
  and upstream diagnostics, instead of graph softmax plus categorical RNG.
- Existing opcode variants are preserved; three new container variants are
  appended. No new execution engine, kernel ABI, dependency or JIT is added.
- The two execution-census fixtures now use still-interpreted vectorized gamma
  calls, so their intended fallback and failed-host-probe coverage remains.

The pinned upstream categorical-logit implementation reads element zero of
an empty cumulative-sum vector without a size check. The initial empty-input
probe crashed. The shared helper now calls upstream `check_nonzero_size`
before invoking that RNG. Empty logit inputs reject without advancing the
stream; the reference test uses the same validation primitive instead of
invoking undefined behavior. This is deliberate hardening, not a claimed
bitwise comparison with an upstream empty-input result. All defined positive
and rejected calls retain exact upstream exception types/messages and state.

Cholesky admission requires one logical column-vector location and a matching
square matrix. Row-vector and array-of-vector overloads and rectangular
Cholesky geometry remain outside this slice. The legacy interpreter's shape
adapter is preserved for those refused layouts; its presence is not proof
of correct support. The existing matrix-shape mismatch guard is now exercised
for both covariance and Cholesky forms. Further shape migration must compare
against CmdStan, not assume that legacy flattening is correct.

## Validation

[Fable review and dispositions](2026-09-28-fable-vector-rng-review.md).

Native Release, Apple Clang 21, Darwin arm64. Same pinned Stan/Stan Math and
CmdStan 2.40 source checkout as the scalar-RNG slice. No compiler source
changed. No tolerance was widened.

- `test_vector_rng`: 4 families × graph/register × lengths 0/1/3; two chains,
  12 changing-parameter calls each; invalid/nonfinite inputs, matrix-factor
  violations, rejection and recovery. Full rows and final engines are bitwise
  equal to the interpreter and direct Stan Math (empty-logit exception above).
  Strict traces reject any MIR entry during compiled output evaluation.
- `vector_rng_reference`: independent CmdStan source/data hashes, three
  deterministic points, complete names, rows, log density and gradient;
  **90 values, maximum 1 ULP** under the 10-ULP gate. This fixture also exercises
  integer draws in a runtime loop and their use as a checked vector index.
- **272/272 native CTest tests passed** before the review follow-up, including
  scalar RNG, write-array, lowering, register/compiler, adjoint,
  structured-loop, callback and execution-census suites. The final run with
  the added effect reference and callback guard is recorded below.
- Complete recorded corpus: **329/329 models**, three points, **1,020,194
  values**. Existing 124 model-specific ULP gates and scaled/structural checks
  pass. Worst scaled error 9.38e-13 (`gpcm_latent_reg_irt`, 7040 ULP); this is
  not a universal 10-ULP result.
- Formatting and the native no-stdio symbol audit passed.
- Installed Python wheel suite passed; installed R package tests passed with
  the new runtime and no skips. Native/JavaScript compiler parity and fresh
  browser execution were not rerun for this native-priority slice.

Logs: `.cache/vector-rng-{tests,reference,ctest,corpus,python-tests,r-tests}.log`.
Independent recording inputs and build metadata: `.cache/vector-rng-oracle/`.

## Native performance

[Raw samples and artifact hashes](data/2026-09-28-vector-rng-performance.json).
Six counterbalanced fresh-process pairs, 200 ms warmup and 250 ms measurement,
public C API through ctypes. `tools/bench_rng_paths.py` reproduces each sample:
pass baseline/candidate dylib and fixture stem, alternating order across
processes. Baseline is the previous committed runtime (`84945f1b`); the new
combined fixture falls back as a whole there and compiles completely here.
Times are microseconds, median ± MAD.

| Combined RNG fixture | Baseline | Candidate |
| --- | ---: | ---: |
| Source to MIR, warmed compiler | 1113.43 ± 4.28 | 1047.64 ± 22.63 |
| Prepare from MIR | 246.73 ± 3.39 | 286.39 ± 4.61 |
| First gradient | 6.40 ± 0.27 | 10.92 ± 0.23 |
| Warm gradient | 0.580 ± 0.007 | 0.603 ± 0.009 |
| First output row | 45.71 ± 1.96 | 36.75 ± 1.44 |
| Warm output row | 19.49 ± 0.52 | 1.814 ± 0.036 |
| 100 warmup + 100 samples + rows | 2177.85 ± 92.21 | 482.65 ± 3.69 |

The targeted warm output path is **10.75× faster**. Fuller preparation costs
39.7 µs more; the 17.7 µs per-row saving repays that in about three rows.
First-gradient phases differ because the baseline output-discovery probe has
already evaluated the log-probability graph; the compiled path skips that
probe. Gradient and source-compiler changes are not targeted improvements.
The short inference example demonstrates the row cost in this model, not a
general sampling speedup.

Existing scalar-RNG and AR1 canaries retain their execution routes. Warm row
ratios candidate/baseline are 1.007 and 0.985, and warm gradient ratios 0.954
and 0.971. Small changes and the unrelated inference differences have no A/A
confirmation and are inconclusive. Peak RSS is within about 0.4% on these
workloads, including Python/compiler memory; retained memory is unmeasured.
Dylib size 40,061,344 → 40,061,264 bytes; gzip 12,361,652 → 12,360,418 bytes.
These are development artifacts, not release packages or size-win claims.

The O0 `gq_rng_udf_effect` regression preserves a nested RNG argument in an
actual UserDefined MIR call. It disproves the review's suspected double
argument evaluation: UserDefined calls bypass the RNG hook. Its added
independent CmdStan reference passes **18 values, maximum 1 ULP**. The
review follow-up also checks accepted mutation outputs bitwise and pins the
empty-logit exception to `invalid_argument`.

## Remaining work

Prioritize shared callback early-return and logical-shape handling, followed
by eligible standalone function compilation and measured kernel tape/factor
costs. Vectorized RNG arrays still need a shared shape and broadcasting
contract. Preparation, partial-environment folding and constrained
initialization remain mandatory interpreter-deletion obligations, even when
cold. Native instruction generation and dispatch-JIT remain tabled.

The next callback investigation reproduced an unsafe runtime-return admission;
[callback return guards](2026-09-28-callback-return-guards.md) record its separate
correctness fix and the shared-exit design boundary.

Final combined validation after the Fable follow-up and callback guard:
**273/273 CTest tests** and **329/329 recorded CmdStan models** passed. Logs:
`.cache/vector-rng-callback-final-{build,ctest,corpus}.log`.

Final performance confirmation after the callback guard repeated the same six
counterbalanced process pairs (retained under `final_confirmation` in the JSON).
Target rows: **19.35 ± 0.44 → 1.767 ± 0.040 µs**, **10.95× faster**;
preparation: **250.05 ± 6.42 → 298.09 ± 7.19 µs**. The added preparation is
repaid in about three warm rows. Warm gradients 0.597 → 0.602 µs are effectively
unchanged at this resolution. Short inference plus rows: 2134.25 → 440.23 µs.
Canary row ratios are 0.978 (scalar RNG) and 0.989 (AR1), with no A/A-supported
claim about small shifts. Final peak RSS is within roughly 0.7% of baseline;
retained memory is still unmeasured. Final dylib: 40,061,568 bytes (+224), gzip
12,360,972 bytes (−680). No meaningful size change is claimed.
