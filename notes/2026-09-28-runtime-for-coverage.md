# Fill existing execution-engine gaps before replacing MirInterp

The objective is predictable native performance. Removing `MirInterp` is useful
only insofar as it removes a performance cliff. A replacement with comparable
cost is not progress. The general typed-value-program proposal is deferred.
Machine-code generation and stencil JIT research remain tabled.

Synchronized upstream: `6ce2018b5382b459666ab32fe0df50d4a6d30ba4`.

## Candidates from the current source

| Gap | Existing engine to extend | Work and limits |
| --- | --- | --- |
| Runtime `for` bounds with fixed-size storage | Register program | Use existing comparisons and jumps; preserve bound evaluation, loop-local initialization and exits. First experiment below. |
| Scalar integer arithmetic inside newly admitted loops | Register program | Reuse exact Stan integer operations or prove ranges. Flat double arithmetic alone does not preserve integer overflow and mirror behavior; the new route conservatively refuses it. |
| Nested arrays passed to solver callbacks | Register program plus callback adapters | The shared compiler already has richer array views than callback admission. Carry complete dimensions and convert storage order in both directions, including derivatives. This needs layout tests, not a new engine. |
| Runtime integer callback arguments or solver controls in generated quantities | Graph/register solver calls | Current packing requires constants. Add runtime integer/control inputs with validation and correct solver lifetime; avoid treating an integer value as a fixed shape. |
| Large constant loop expansion | Structured loops or register back edges | Avoid excessive prepared instructions where the body has fixed storage. A separate admission/profitability change: do not merely raise the register limit. |
| Dynamic multidimensional selectors and integer-array writes | Register/structured-loop operations | Extend checked indexing and integer semantics; preserve aliasing, range errors and reverse accumulation. Not every selector requires changing-size storage. |

Changing-size storage and genuine recursion remain more general problems.
Preparation-time interpretation may be inexpensive enough to keep. Prioritize
frequently repeated fallbacks, especially callbacks evaluated many times by a
solver and output blocks that fall back as a whole.

## First experiment: runtime for loops

**Hypothesis:** a callback with fixed-size inputs, outputs and locals can stay
in the register engine when only its trip count changes. This avoids repeating
MIR traversal and name lookup on every solver callback without introducing an
engine, opcode or new derivative algorithm.

**Counterproposal:** rewrite the source loop as an equivalent `while`, already
supported by the register engine. This identifies an existing runtime mechanism
we can reuse automatically rather than asking users to discover that rewrite.
General dynamic storage does not address the immediate refusal.

**Baseline:** `ode_runtime_for.stan` runs today, but its ODE callback selects
`mir_interpreter` because the bound cannot be evaluated during preparation.
Saved baseline binaries and initial measurements live in
`/tmp/stanli-runtime-for`. The independent CmdStan reference was recorded before
changing production lowering. A first single-process sample measured about
396 microseconds per gradient and 447 milliseconds for 100 warmup + 100 retained
draws and their output rows. These are preliminary observations, not paired
performance results.

**Implementation boundary:** retain constant-bound unrolling; compile unknown
bounds into captured registers, a counted loop and the existing function exit
mechanism. Fixed storage remains mandatory. Integer local declarations must
initialize on each trip. Apply the same conservative integer-arithmetic check already used by the
standalone API, including enclosing function bodies and called functions.
Integer comparisons, shapes, literal assignments and loop counters are
supported; the upper bound must be invariant and effect-free; general integer arithmetic is not newly admitted. Initially refuse
integer-array mutation, unsized container adoption and runtime `for` nested in
`while`, whose folded local bindings need a separate lifetime proof. Nested
runtime `for` and `while` inside runtime `for` are tested with real accumulators
and counters. Scalar integer predicates declared inside the loop initialize
on every trip. This patch requires a checked function scope; it does not
change top-level generated-quantities loop routing.

**Evaluator:** require bitwise value and weighted-gradient agreement with
`MirInterp`; test empty/reversed ranges, bound mutation, changed calls, nested
loops, local integers, early returns, break/continue and a final INT_MAX trip.
Preserve refusal for runtime-sized local containers. Replay an independently
recorded CmdStan model through density, gradients and output rows, with explicit
engine-selection assertions. Measure six alternating native baseline/candidate
pairs for warm gradients, preparation, first calls, output rows, short inference
and process memory, plus an unrelated already-compiled callback canary. Report
binary growth. Run focused compiler/loop/adjoint checks, then the configured
native tests and recorded numerical corpus before integrating.

## Correctness results

All 293 configured native CTests pass. The recorded corpus passes all 329 models
at three points: 1,020,194 compared values and all 124 platform ULP gates. The
worst corpus difference remains 9.38e-13 scaled error / 7040 ULP; this is not a
universal ten-ULP claim. The new independently recorded CmdStan 2.40 fixture
matches all 21 density, gradient and output values exactly (0 ULP).

The compiled callback tests compare values and weighted derivatives bitwise
across twelve changing inputs per accepted function. Additional checks cover
ordered lower-bound effects even for an empty loop, effectful/mutated upper
bound refusals, the INT_MAX endpoint against
MirInterp (not an upstream signed-overflow claim), changing local shape,
integer overflow and integer-array mutation refusals. Reviewer follow-ups
assert exact refusal reasons, no-loop overflow refusal, loop-carried-value
compaction and runtime indexing of constant callback integer data. Engine diagnostics
assert register selection in both density and output callbacks.

The public C API comparison matches 642 doubles bitwise: six changing parameter
calls, 100 posterior draws after 100 warmup iterations, and every draw's output
row. Installed Python tests pass; installed R tests pass with the current
native runtime and stock compiler supplied. No missing-runtime tests are
skipped. The portable compiler branch was not an additional target here.

## Review

Fable reviewed the plan and implementation through the Claude CLI; the
[review and dispositions](2026-09-28-runtime-for-fable-review.md) record the
findings and follow-up tests. Its loop lifetime, bound-copy,
integer-array folding and exit observations informed the implementation.
Its claimed reachability blocker was incorrect: a callback can select an
integer bound from a real state value, without any runtime integer callback
formal. The checked-in fixture and before/after engine diagnostics demonstrate
that route. No generated-quantities routing change was needed for this fix.

A final upstream source check found a pre-existing semantic mismatch:
`MirInterp` captures an upper bound once, while pinned stanc emits a C++ loop
condition that can re-read it. The new route therefore requires an invariant,
effect-free upper bound and refuses mutation or effectful calls there. It
accepts only the semantics shared by both implementations. Lower-bound effects
still execute once. This patch does not repair the existing fallback's
upper-bound behavior; that needs a separate consistency fix across engines.

## Native performance

Final guarded candidate versus the saved baseline, same native Release build
configuration on Darwin arm64 / AppleClang 21. Six alternating fresh-process
pairs per model, 200 ms warmup and 250 ms measurement windows. The phase tool
uses the public C API (including Python call overhead); the native gradient
benchmark avoids that overhead. Short inference includes 100 warmup iterations,
100 retained draws and all output rows. These are small coverage fixtures, not
a broad model-performance claim.

| Median phase | Runtime-for callback before → after | Already-compiled callback before → after |
| --- | ---: | ---: |
| Source compilation | 696 → 699 µs | 516 → 527 µs |
| Preparation from MIR | 271 → 279 µs | 306 → 324 µs |
| First gradient | 460 → 45.9 µs | 41.7 → 40.9 µs |
| Native warm gradient | 398 → 4.99 µs | 1.82 → 1.86 µs |
| First output row | 726 → 24.9 µs | 23.3 → 19.2 µs |
| Warm output row | 734 → 5.38 µs | 1.79 → 1.72 µs |
| Inference and output rows | 446 → 5.45 ms | 3.44 → 3.44 ms |
| Whole-process peak RSS | 28.6 → 28.5 MB | 28.3 → 28.3 MB |

The target fixture improves 79.6× in native warm gradients and 81.8× in short
inference with outputs. Its first gradient improves 10×. The extra preparation
cost is reported rather than assumed away. Source compilation uses the same
embedded compiler; these samples do not attribute its small timing differences
to a compiler change.

Before the final upper-bound guard, the ordinary canary's native gradient
increased about 3.8% while its short inference stayed effectively flat. A
predeclared eight A/A and eight A/B native-gradient control gave median paired
ratios of 1.004 (MAD 0.010) for A/A and 1.001 (MAD 0.025) for A/B. Those ratios
overlapped; the control did not reproduce a consistent slowdown. This is not
proof of zero regression. The final-bound-guard samples are reported in the
table above. The earlier signal and controls remain separately identified by
binary hash in the artifact, not relabeled as tests of the final binary.

The shared library grows 19,152 bytes uncompressed and 7,651 bytes gzip. There is
no new dependency or execution engine. Peak RSS includes compiler/allocator
state and does not establish retained-memory changes. Raw measurements,
medians, dispersion, binary/source hashes, engine reports and exact public-API
comparisons are in [the performance artifact](2026-09-28-runtime-for-performance.json).

Reproduce phases with `tools/bench_model_phases.py LIBRARY MANIFEST MODEL`
and native gradients with `build-release/bench_grad MIR DATA --timed
--warmup-ms 200 --measure-ms 250`; the artifact contains the manifest and
baseline/candidate identities. Alternate baseline/candidate order over six
fresh-process pairs. Reproduce numerical checks with `ctest --test-dir
build-release --output-on-failure`, `tests/test_scalar_rng_reference.py
build-release/stanli_check ode_runtime_for 0`, and `tools/verify_refs.py
/Users/xitrium/claud/stanrt/deps/posteriordb --check build-release/stanli_check
--jobs 4` (invoke the Python tools with Python 3).

## Next coverage work

Prioritize exact integer operations and callback container layouts before any
new general-value engine. These would allow more ordinary fixed-size loops and
functions to stay compiled. Follow with runtime integer/control packing and
broader checked selectors. Profile preparation and truly dynamic storage cases
before deciding whether their interpreter use is a performance problem.


