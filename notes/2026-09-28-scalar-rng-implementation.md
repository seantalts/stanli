# Scalar RNG compiled coverage

Continuation of the [reviewed execution roadmap](2026-09-28-execution-engines-and-roadmap.md).
The user authorized continuing until completion or a major design decision.
Fetched `origin/HEAD` remained `5b913866768f351f86eed4d5a7246414530f3b2d`;
no integration conflict. The census was checkpointed as `3f5ef4a8` before this slice.

## Contract and implementation

The eleven scalar families previously limited to MIR interpretation now lower
through the shared registry to `OP_RNG`, including register-program `CALL`s:
`std_normal`, `gamma`, `inv_gamma`, `beta`, `chi_square`, `cauchy`,
`double_exponential`, `logistic`, `weibull`, `neg_binomial_2`, and
`neg_binomial_2_log` (all with `_rng` suffixes).

Each family calls the existing upstream Stan Math algorithm on the caller-owned
stream. Existing variant numbers, including the three reserved container
variants, are unchanged. The interpreter now uses the same family dispatcher;
its element traversal is unchanged. Graph lowering handles zero arguments for
`std_normal_rng`. Both lowerers accept explicit MIR integer-to-real promotion
of integer draws, while rejecting incompatible result types.

The eligibility proof is deliberately scalar: validated family, arity, result
kind and scalar arguments. New vector/array forms retain the interpreter until
their validation/broadcasting order is separately established. No source/model
names select production behavior. A container near-miss fixture verifies the
remaining fallback and its exact stream continuation.

Census fixtures now use the still-interpreted `poisson_binomial_rng` family.
One old lowering regression used unsupported gamma as an accidental way to force
interpretation; it now uses the explicit test-only force-interpreter switch.
Neither change weakens the behavior the tests check. Historical census JSON
remains a snapshot of its identified earlier tool/MIR artifacts.

## Validation

- Full Release native suite: **270/270 passed**. After adding the container
  near-miss, its focused test, the independent reference replay and the complete
  fixture cross-path test passed again.
- New differential matrix: all eleven families through both ordinary graph and
  runtime-loop register lowering; 12 successive changing-parameter draws per
  seed/chain, chains 0 and 3, exact row bytes and engine state. Invalid, infinite,
  NaN and boundary inputs compare exception types/messages, stream positions and
  recovery against both the interpreter and direct upstream calls. Strict trace
  scopes prove the compiled evaluations do not construct a MIR interpreter.
- Independent pinned CmdStan 2.40.0 recording at all three standard points:
  **54 values, maximum 1 ULP**, complete stochastic rows and exact column names.
  Recording used `verify_sample.build_ref` and `strict_outputs` before inspecting
  candidate output, seed 1234 / chain 0. Source/data/driver hashes and all pins are
  in `tests/fixtures/gq_scalar_rng_complete.ref.json`. The initial exploration
  with CmdStan 2.39 was replaced by a fresh pinned 2.40 recording, not relabelled.
- Full existing corpus: **329/329**, 1,020,194 values, all three points; existing
  scaled/structural and applicable 124-model ULP gates passed. This remains a
  different contract from a universal 10-ULP guarantee.
- Freshly installed Python wheel: all 55 checks passed. Installed R suite passed
  with local runtime, stock compiler and CLI configured, without skipped tests.
- The builtin signature manifest was regenerated from the new registry. RNG
  signatures remain explicitly excluded from pure-function generated models;
  the dedicated seeded tests provide their numerical evidence.

Logs: `.cache/scalar-rng-final-ctest.log`, `scalar-rng-guard-tests.log`,
`scalar-rng-corpus.log`, `scalar-rng-python-tests.log`, `scalar-rng-r-tests.log`.
The default runtime and C ABI were exercised; native/browser compiler parity
was not rerun, and no compiler source changed.

## Performance evidence

[Raw samples, medians/MAD and artifact hashes](2026-09-28-scalar-rng-performance.json).
Six alternating fresh-process baseline/candidate pairs used the public C API via
ctypes. Warm measurements had 200 ms warmup and 250 ms measurement per process;
first calls and short inference runs have six process observations. Times below
are microseconds, median ± MAD. Source compilation uses a warmed embedded
compiler, not a cold installation. The comparison is census checkpoint versus
this scalar patch on the same host, compiler, pins and Release configuration.

| Eleven-family model metric | Baseline | Candidate |
| --- | ---: | ---: |
| Source to MIR | 716.98 ± 14.29 | 718.60 ± 20.70 |
| Prepare from MIR | 216.43 ± 2.87 | 195.53 ± 2.06 |
| First gradient | 5.92 ± 0.08 | 11.60 ± 1.00 |
| Warm gradient | 0.581 ± 0.007 | 0.593 ± 0.013 |
| First output row | 33.75 ± 0.96 | 22.69 ± 1.08 |
| Warm output row | 11.36 ± 0.11 | 1.276 ± 0.033 |
| 100 warmup + 100 samples + output rows | 1373.81 ± 46.06 | 401.94 ± 22.63 |

The targeted output path is **8.9× faster** in this workload. Preparation is
cheaper because interpreted output discovery is unnecessary. That discovery
previously evaluated the log-density graph to obtain constrained values; the
baseline's first public gradient is therefore partly warmed already. This
explains a phase-boundary difference to investigate before attributing its
roughly 6 µs increase to gradient execution. Preparation plus first gradient is
still lower here. Cold-call measurements are noisy and not a general latency
claim.

Existing scalar-RNG and 24-parameter AR(1) canaries retain their engines. Warm
row candidate/baseline ratios were 0.964 and 1.028; warm gradient ratios 0.985
and 1.014. These small shifts are inconclusive, not proof of zero regression.
Peak process RSS rose about 0.4% for the target, with near-flat canaries; it
includes Python/compiler memory. Retained-memory attribution was not measured.
Installed dylib size rose from 40,045,904 to 40,061,344 bytes (+15,440 bytes).
Compressed sizes are in the JSON; they are development artifacts, not released
package sizes. No inference-wide speedup is claimed beyond this small measured
model.

## Next research boundary

The scalar tranche is ready. The roadmap calls for an early code-generation
feasibility comparison before expanding every remaining value path. Developer-
only native and Wasm probes are separate from this production change. Their
results will inform the major choice of generated-model/kernel interface and
backend; they do not authorize deleting existing execution engines.
