# Container RNG arguments in the existing engines

Status: tested and accepted for adoption, **not merged**. The implementation
passes correctness checks and improves repeated output execution. The user
accepted the measured preparation tradeoff: prevent regressions in normal use,
rather than requiring every individual phase to become faster.

Baseline: fetched `origin/HEAD` at `df86223160f1387a37ed47c267df1bf31e77f11c`.
`df91dbe6` checkpoints the audit and plan without runtime changes. Release,
Darwin arm64, AppleClang 21; full native runtime with threads enabled. Binary
hashes, inputs, phase medians/MAD, profiles and all six alternating sample pairs
are in the [performance record](data/2026-09-29-container-rng-performance.json).

## Change and proof

The graph and register compiler admit fixed-size one-dimensional arguments to
all 22 container-capable families in the existing scalar RNG vocabulary. Arrays
of integers/reals, vectors and row vectors retain their language identity;
an array of one element is not a scalar broadcast. `std_normal_rng()` remains
the existing nullary scalar call.

A container call uses one existing `OP_RNG` operation, with a scalar/container
argument mask in its existing metadata. There is no additional execution engine,
instruction record enlargement, runtime switch or disabled prototype. The
scalar call keeps its existing helper and no-metadata path. Graph lowering also
removes the old sequence of per-element draws, indices and repeated concatenations.

Graph, register and MIR execution share a helper that calls Stan Math's actual
vectorized RNG overloads. Those overloads preserve whole-argument validation,
scalar broadcasting, empty containers, draw order and the continuation stream.
Real containers use Eigen views; population counts are converted to Stan's
integer container. Stan owns the numerical algorithms and result generation.

The previous MIR expansion performed scalar draws one element at a time. That
was insufficient for validation before drawing, empty inputs and container
length checks. The shared helper fixes those semantics too. It is an oracle
comparison, not an assertion that preserving the old fallback is always correct.

Compile-time eligibility requires valid scalar/one-dimensional source types,
matching non-scalar extents and fixed storage. Mismatched lengths retain a
fallback which raises Stan's error at evaluation; genuinely changing storage
remains unsupported by this graph/register extension. Successful integer draws
are initialized runtime values, never compile-time geometry.

## Correctness and coverage

- A new model exercises all 22 families with array/vector/row-vector arguments,
  both ordinary graph calls and calls inside a runtime loop. Independently
  recorded CmdStan 2.40.0 outputs, density and gradients at three parameter points
  pass: **450 values, maximum 1 ULP**. The trailing draw checks continuation.
- Focused tests compare graph, register, explicit MIR and direct Stan Math calls
  at sizes 0, 1 and 3, multiple seeds and repeated calls. They check broadcasting,
  invalid later elements, nonfinite arguments, mismatched lengths, error class/
  message and the complete RNG state after success or rejection.
- Existing malformed-MIR checks still refuse inconsistent argument/result types.
  Fallback diagnostics now use genuinely runtime-sized local storage; a supported
  RNG call is no longer an appropriate refusal fixture.
- **312/312 CTests pass.** The recorded CmdStan corpus passes **329/329 models,
  1,020,194 values**, retaining all existing numerical gates and exceptions.
  The historical 7040-ULP cancellation case remains; this is not a universal
  ten-ULP claim.

## Native measurements

Six alternating fresh-process pairs using `tools/bench_model_phases.py`; no
concurrent build, tests or benchmark. Values below are medians in microseconds.
Inference means 100 warmup iterations plus 100 draws and their output rows;
source compilation and preparation are reported separately in the artifact.

| Workload | Preparation, before → after | Output row, before → after | Inference and rows, before → after |
| --- | ---: | ---: | ---: |
| Integer-array binomial, 1 element | 270.4 → 253.6 | 5.00 → 0.60 | not measured |
| Integer-array binomial, 32 elements | 277.8 → 242.6 | 7.33 → 2.17 | 3,910 → 3,049 |
| Integer-array binomial, 1,024 elements | 432.1 → 350.9 | 74.11 → 56.28 | not measured |
| All 22 families, graph plus runtime loop | **571.0 → 745.5** | **94.53 → 10.22** | **10,251 → 1,292** |

The empty-array baseline fails output evaluation, so there is no legitimate
before/after latency ratio. The candidate evaluates it successfully; the focused
upstream tests independently establish empty-input values and stream behavior.

The AR(1), scalar-RNG and vector-RNG canaries show no clear slowdown relative to
paired timing variation. Their preparation ratios are 0.999, 0.996 and 1.006;
paired MADs are 0.079, 0.067 and 0.050. Output ratios are 0.997, 1.003 and 1.016
with MADs 0.039, 0.017 and 0.057. These finite measurements do not prove no
regression for every possible model or machine. Full first/warm gradient and
output measurements, source compilation, inference and RSS are retained.

The native library grows by **106,000 bytes (0.27%)**, from 39,707,984 to
39,813,984 bytes. Mixed-fixture peak process RSS rises from about 29.14 MB to
29.72 MB; the other cases are close to baseline. Peak RSS includes the Python
process and mapped code and does not establish retained executor memory.

## Decision: judge normal end-to-end use

The mixed fixture saves about 84 microseconds per output row but adds about
175 microseconds of preparation. It therefore recovers the extra preparation
after roughly **three rows**. Preparation plus just one row is slower, even
though complete inference with output is substantially faster. No universal
no-regression claim is justified.

Preparation profiling locates the increase mainly in lowering the previously
interpreted output body. The candidate prepares all its calls and output views;
the baseline refuses early and leaves the body to MIR. The profiles also show
more optimization work, but eliminating that alone cannot erase the difference.
This is not new overhead on each unaffected scalar numerical instruction.

The user chose to judge promotion on normal complete workloads, reporting the
startup cost and break-even point. This candidate is accepted under that policy;
the one-row limitation remains visible. This also governs later coverage work:
measure normal use and preserve established fast paths, rather than requiring
monotonic improvement in every phase. A delayed or adaptive preparation policy
would need a separate design; none is added here.

The [working checklist](2026-09-29-execution-coverage-checklist.md) retains integer
expressions, standalone container adapters and callback gaps as the next direct
extensions. The research into native instruction generation remains tabled.

## Reproduction

```sh
cmake --build build-release -j4
ctest --test-dir build-release --output-on-failure -j4
python3 tools/verify_refs.py /path/to/posteriordb --check build-release/stanli_check --jobs 4
python3 tests/test_scalar_rng_reference.py build-release/stanli_check gq_container_rng_complete
python3 tools/bench_model_phases.py /path/to/libstanli.dylib tools/bench_execution_paths.json gq_container_rng_binomial
python3 tools/bench_model_phases.py /path/to/libstanli.dylib tools/bench_execution_paths.json gq_container_rng_complete
```

Use alternating baseline/candidate processes for six pairs, as specified in the
[benchmark protocol](../../docs/benchmark-protocol.md#focused-execution-path-benchmarks).
For boundary sizes, vary `K` and set `trials[i] = 5 + i % 8`; use
`--skip-inference` for the 0/1/1,024-element checks. The maintained manifest now
includes the two new output workloads. Public corpus benchmark tables are unchanged.
