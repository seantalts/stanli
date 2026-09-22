# Issue 374: numerical agreement before further speed work

Current work starts from main `d73a358324233a2b62e97fbaa23dcbcdf3305dd0`.
The user asks to fix the numerical discrepancies first, then bring the brms
models to a CmdStan/Stanli runtime ratio of at least **0.8**. Larger ratios
favor Stanli. The objective includes the models still outstanding in #374,
not just the thirteen measured in the earlier refresh. The bounded follow-up
below now brings the three previous timing misses above 0.8 in a fresh
focused sampling run; the earlier full sweep is retained unchanged.

## Evaluator

- Compare densities, every gradient, and per-draw outputs at the three
  established points against the independent, matched Stan/CmdStan 2.40
  reference. Start with GEV and the ordinal exceptions; inspect individual
  values rather than accepting the broad scaled-error gate. Aim to bring
  these discrepancies within 10 ULP without changing references or tolerances.
- Reduce each cause to a focused regression test against the upstream Stan
  Math expression. Cover activity, shape, cancellation, and exceptional
  inputs that distinguish the relevant overloads. Preserve correct fallbacks.
- Once answers agree, measure warm gradients and complete sampling separately.
  Count preparation in Stanli's sampling time, as in the original issue;
  exclude CmdStan's ahead-of-time compilation from that comparison. Retain
  all seeds and diagnostic failures. Do not infer useful inference from
  pathological chains, or claim all-model coverage from a selected subset.
- Check ordinary models for preparation, first-gradient, runtime, and memory
  regressions; run the focused tests and full configured correctness suite
  before landing. Numerical fidelity remains the first gate.

## Causes established by differential experiments

The initial GEV discrepancies were 25/77/256 ULP across the three points.
The ordinal gradient discrepancy reached 5,861 ULP. They were not one
approximate distribution formula: several transformations changed floating
point grouping, with cancellation exposing their combined effect.

- Keep stanc3's stability-preserving O1 policy and disable partial evaluation
  that introduces fused arithmetic absent from the reference compiler.
  Explicit source `fma` still means `fma`.
- Select scalar and matrix division and scalar constant-power overloads from
  source types and activity. Preserve Stan Math's multiplication/division
  grouping and the reverse order of repeated source scalar calls.
- Match the scalar adjoint view in a data-matrix/active-vector product.
  A contiguous Eigen double reduction can choose a different grouping.
- Fold target additions in source order, including retained loop targets.
- Cache repeated active expressions' forward values, while retaining separate
  adjoints and each original reverse callback. Combining their gradient seeds
  before a nonlinear callback is not numerically equivalent. Apply the same
  rule to loop-invariant calls. Inactive common expressions still share both
  their computation and output.
- Continue incoming adjoint accumulation across a compiled region when its
  input cells are immutable and distinct. Mutable or overlapping inputs keep
  the established additive boundary; overwriting an external gradient with
  one of several local contributions would be incorrect.
- Keep non-exp-quad GP covariance construction and an ascending scalar
  diagonal update on the same Stan Math tape. Those covariance implementations
  share diagonal scalar nodes; separately seeding the matrix changes reverse
  accumulation order. Fusion requires a complete, unobserved update chain.
- Preserve interleaved updates for a matrix plus its own transpose: both
  operands share Stan scalar nodes. Other matrix additions keep their existing
  kernel, and the aliasing variant is not widened into independent lanes.
- Preserve the separate overloads for `dot_self` and data-Eigen `sum`, including
  generated-output interpretation. Active sums and real-array sums retain
  their scalar order.
- Refuse lane fusion when the same shared active scalar occurs at multiple
  positions in its repeated body. Grouping these updates by operation would
  reorder the source's interleaved pullbacks. The scalar fallback remains.

None of these choices depend on a model name or source fingerprint. Focused
regressions cover cancellation, non-unit incoming gradients, scalar/vector
activity, aliases, repeated evaluation, clone independence, and refusal cases.
A full-corpus check also exposed a pre-existing island-carver liveness bug:
renaming an updated vector must carry its last-use information into later
region analysis, or a subsequent update can be discarded.

## Numerical evaluator and scope

The existing reference replay now enforces a maximum of 10 ULP for each of
all 124 brms fixtures, at each of the three recorded points, across density,
gradients and per-draw outputs. References and existing scaled tolerances are
unchanged. Rejections, widths and non-finite classifications remain checked.
This finite corpus contract is not a bound over arbitrary parameter values,
and does not assert a 10-ULP maximum for every other collection.

Raw experiments, compiler ablations, baseline binaries, exact build commands,
model-level values and performance observations are retained during this work
under `.cache/issue374-numerics-20260921/`. Validation after integration:
**266/266 CTests** and **329/329 recorded model
replays** pass. The replay compares 1,020,194 values under the documented
per-fixture gates. Within the 124 brms fixtures, the finite comparisons are:

| Quantity | Values | Exact | Maximum ULP |
| --- | ---: | ---: | ---: |
| Log density | 367 | 323 | 2 |
| Gradient | 5,079 | 4,954 | 10 |
| Per-draw output | 9,928 | 9,928 | 0 |

Five of the 372 model-points reject or return non-finite results; those are
covered by the replay's behavior checks rather than counted as finite ULP
comparisons. The GEV and ordinal discrepancies that motivated this work now
match exactly at the recorded points. Other corpus collections retain their
existing gates; the full corpus still contains cancellation cases above 10 ULP.

The current build uses matched CmdStan/Stan Math 2.40. The CmdStan revision is
`d3d5df6a22565edbe13edbd4eb40762cc8c5a4d6`. Release builds use `-O3` with
floating-point contraction disabled for the numerical/gradient drivers.
The `Op` record remains 80 bytes. The local shared library grows by 36,528
bytes (39,962,560 to 39,999,088, about 0.09%); these are uncompressed development artifacts,
not a measurement of release wheel or browser download sizes.

The OCaml compiler tests, 51 reference-verifier tests, and native/JavaScript
compiler byte-parity checks also pass. The optional stock JavaScript fallback
was not supplied to the parity test; this is not a new browser-runtime or
installed-package certification.

## Performance changes retained

- Evaluate a repeated, nontrivial scalar function argument once when inlining.
  For example, an argument containing a division should not be recomputed in
  the condition and both branches of the function. This uses an opt-in patch
  to the pinned stanc3 inliner; stock-compatible compiler entry points keep
  their original setting. Native and JavaScript producers use the same patch.
- Store only the used part of an input vector before assigning a compiled
  region's register storage. Preserve the original descriptor offsets when
  several inputs are packed together.
- Keep cached forward values out of the executor's forward dispatch list.
  Each original reverse callback remains in source order. Profiling gets its
  own opcode map only when enabled; ordinary executors no longer retain the
  old second-output pointer array.
- Remove unreachable instructions and provably overwritten constant stores
  from small, acyclic branch programs. Preserve arithmetic even when its
  result is unused, because a zero incoming gradient can still interact with
  a non-finite derivative. Unknown ranges and loops keep the old path.

Binding repeated arguments reduced warm gradient time by approximately 18%
for `sw_asymlaplace` and 16% for `s2_zi_asymlaplace`, compared with the preceding
numerically corrected implementation. That comparison is separate from the
main-branch comparison below. The other runtime changes mainly reduce stored
registers or help the ordinal models; they did not independently close the
Laplace gap.

Experiments with prebound island adjoints, extra small compiled regions,
scalar addition/subtraction callbacks, and skipping empty structured-loop
forward instructions did not show sufficient improvement and were removed.
Fully expanding the GEV loop was slower. These experiments and their negative
results remain in the local research packet, not in the production path.

## Complete sampling comparison, September 22

All **124 brms fixtures** and the noncentered eight-schools control completed
four seeds in both engines, with 1,000 warmup iterations and 1,000 retained
draws per seed. No engine failed or timed out. Engines ran serially, alternating
order by seed, with numerical-library thread counts set to one. This was an
Apple M3 Ultra shared desktop; this task ran no concurrent builds, tests, or
other timing jobs. The input files are the existing corpus fixtures, not a
claim about every possible brms model or data size.

The headline ratio is **median CmdStan process time / median Stanli process
time** across the four seeds. It includes Stanli source compilation,
preparation, sampling and output. CmdStan's C++ compilation is excluded.
This is a complete sampling measurement, distinct from the setup-plus-gradient
estimate in `docs/benchmark-protocol.md`.

**121/124 brms models meet the requested 0.8 ratio. The median ratio is 1.29.**
The three remaining misses are:

| Model | Stanli wall time, ms | CmdStan wall time, ms | CmdStan / Stanli |
| --- | ---: | ---: | ---: |
| `sw_asymlaplace` | 142.777 | 111.323 | 0.780 |
| `s2_zi_asymlaplace` | 180.462 | 140.450 | 0.778 |
| `s2_gev` | 127.352 | 88.483 | 0.695 |

The target is therefore not met for every fixture. GEV's retained sampling
step counts match CmdStan exactly in all four seeds, so that gap cannot be
attributed to fewer sampler steps in CmdStan. These short process timings also
vary across runs; the earlier, smaller sweep put GEV at 0.841. Retain both
observations rather than choosing the favorable one.

The [model-by-model results](2026-09-21-issue-374-results.csv) include the
headline ratio, median/MAD of paired ratios, peak process memory, numerical
maxima, and diagnostic flags. A blank ULP cell means no finite recorded
comparison for that quantity; rejection checks still apply. The runtime executables retained after the
discarded experiments have the same SHA-256 hashes as those in this full sweep.

Forty-eight brms fixtures had a convergence warning in at least one engine:
divergences, maximum tree depth, R-hat above 1.01, undefined diagnostics, or
minimum bulk effective sample size below 400. Keep their raw timings, but do
not interpret them as evidence of equally useful posterior samples. Of the
76 fixtures without a warning, 73 meet the speed target; their largest
between-engine posterior-mean difference is 0.14 pooled standard deviations.
The eight-schools control had two divergences in each engine and is excluded
from these brms counts.

Six alternating fresh-process gradient pairs were also measured for the
three misses, two ordinal cases and three controls. Each pair used a 200 ms
warmup and at least 500 ms of measurement. Every density/gradient comparison
was within 4 ULP at the timed point.

| Model | Stanli gradient, µs | CmdStan gradient, µs | CmdStan / Stanli |
| --- | ---: | ---: | ---: |
| `sw_asymlaplace` | 4.611 | 3.350 | 0.726 |
| `s2_zi_asymlaplace` | 4.967 | 4.397 | 0.885 |
| `s2_gev` | 6.468 | 4.904 | 0.758 |
| `sw_cratio` | 61.564 | 75.134 | 1.220 |
| `sw_cumulative_cs` | 61.536 | 62.045 | 1.008 |
| `sw_gaussian` | 0.356 | 0.418 | 1.176 |
| `sw_bernoulli` | 0.454 | 0.546 | 1.204 |
| `eight_schools_noncentered` | 0.234 | 0.416 | 1.778 |

## Costs relative to main

Six alternating process pairs compare main `d73a3583` with this change.
Both use their own compiler-produced MIR. Preparation below means loading
MIR/JSON through a bound executor; source compilation is excluded from this
table and included in the complete sampling measurements above. The first
gradient is timed separately, before warmup. Memory is peak resident process
memory, not a measurement of retained heap allocations.

| Model | Preparation, ms | First gradient, µs | Warm gradient, µs | Peak RSS, MiB |
| --- | ---: | ---: | ---: | ---: |
| `s2_gev` | 0.994 → 1.013 | 138.479 → 142.750 | 6.518 → 6.408 | 6.461 → 6.414 |
| `sw_cratio` | 14.171 → 14.468 | 72.625 → 74.021 | 60.622 → 62.519 | 14.875 → 15.305 |
| `sw_cumulative_cs` | 50.011 → 49.437 | 81.416 → 88.125 | 50.067 → 62.111 | 12.781 → 15.023 |
| `sw_asymlaplace` | 1.698 → 1.660 | 19.459 → 18.896 | 4.527 → 4.492 | 6.703 → 6.797 |
| `sw_gaussian` | 0.596 → 0.582 | 15.146 → 14.958 | 0.362 → 0.361 | 5.391 → 5.438 |
| `sw_bernoulli` | 0.492 → 0.510 | 12.729 → 13.042 | 0.453 → 0.452 | 5.070 → 5.195 |
| `eight_schools_noncentered` | 0.381 → 0.408 | 9.312 → 9.000 | 0.230 → 0.235 | 4.766 → 4.836 |

The small controls' warm times differ by roughly 0–2%, with preparation
differences of 0.01–0.03 ms. Peak RSS is slightly higher for those controls.
This is not evidence of zero cost in every phase. The material regression is
`sw_cumulative_cs`: preserving source-order reverse accumulation costs about
24% in warm gradient time and 2.24 MiB of peak RSS. Its complete sampling ratio
against CmdStan is still 1.018. The old, faster result failed the numerical
agreement goal; restoring that optimization is not an acceptable speed fix.

## Retained evidence and next investigation

The compact CSV is checked into `notes/`; executable regression fixtures stay
under `tests/`. The local `.cache/issue374-numerics-20260921/` packet contains:

- `brms-numerics.json`, `corpus-shipping.log`, `ctest-bound-final.log`,
  `compiler-bound-tests-final.log`, and `compiler-parity-shipping.log`;
- `sampling-final.json`, its manifest, all CSV draws and stderr under
  `performance-final/`, and `diagnostics-final.json`;
- `gradients-final.json` and its manifest, `phase-retained-final.json`,
  source/binary snapshots, and the discarded optimization comparisons.

The packet retains source/data/MIR/executable hashes, compiler provenance,
commands, seeds, environment, timeouts and machine identity. Numerical
references were not regenerated. The full-sweep executables are
`bench_grad` SHA-256 `9f7ab3aa4a2322521aed38627cb37b49e6d63603376016de44f05d2cd9092a44`
and `stanli_run` SHA-256
`be8a092efd2ba9d144bbb0c42a3a341bd733093e3042e16723a59c5e73a8a583`.

Further speed work should focus on executing the scalar branch/loop programs
with less callback and argument-binding overhead, while preserving each
source operation's separate gradient update. In particular, combining
gradients of repeated expressions is not a valid shortcut back to the old
speed. The three misses above and the ordinal memory cost are still open.

## Bounded follow-up, 2026-09-22

The upstream inliner draft is [stanc3 #1726](https://github.com/stan-dev/stanc3/pull/1726).
The user requests a modest additional pass on the three remaining models.
Base remains `d73a358`; the before-state binaries, patch, profiles and new
measurements are isolated in `.cache/issue374-small-perf-20260922/`.

Fresh sampling profiles attribute much of the asymmetric Laplace cost to
the expression interpreters, and about half of GEV to structured replay
itself. Per-opcode timers exaggerate the cost of tiny callbacks, so their
percentages are not used as savings predictions. The ALD island contains
19 forward and 10 reverse instructions, including clears of private
constant/comparison adjoints that no earlier source operation can read.

Two bounded hypotheses: (1) omit those terminal clears only at the first
definition of a private register, preserving clears for overwritten inputs
and shared cells; (2) unroll the common one/two-input replay binding without
changing tape layout or arithmetic. The first has roughly 5–10% plausible
model headroom; the second has an uncertain, likely smaller gain. Keep a
change only after alternating before/after measurements on all three targets
and small controls, bitwise internal comparisons, the existing 10-ULP brms
oracle, phase/memory checks and the relevant suites. Stop after these probes
if gains are noise or need a larger representation change. Fusing whole
piecewise likelihoods could remove more dispatch, but is deferred because
its control-flow and adjoint-order proof exceeds this request.

Inspection also found that the previous forward-context compaction left
copy-on-write data rebinding indexed by graph order. A focused clone/data
mutation regression and correction are required independently of timing.

### Follow-up result

Keep both small changes. The private-clear omission reduced each ALD
island's reverse program from ten instructions to five. It does not remove
forward evaluation, validation or effects, and does not combine gradient
contributions. First definitions that overlap a live input or a protected
adjoint range retain the old clear; later writes retain it too. Existing
branch/reuse/fuzz tests and new active-overwrite cases exercise the boundary.
`STANLI_NO_PRIVATE_ADJOINT_CLEARS=1` restores the previous generation.

Structured replay now spells out the common one/two-input pointer binding;
other arities keep the loop. It changes only assignments to independent
context fields, including resolving imported adjoints, before calling the
same kernel. Dynamic lengths, outputs, scratch, guards, reverse order and
tape storage are unchanged. The ablation is the saved
`private-clears/bench_grad` versus the final binary; no permanent tape field
or runtime toggle was added for this specialization.

Six alternating 200-ms warmup / 500-ms measurement pairs gave:

| Isolated change | Model | Before / after ns per gradient | Paired speedup median ± MAD |
| --- | --- | ---: | ---: |
| Omit unused clears | `sw_asymlaplace` | 4633 / 4316 | 1.070 ± 0.005 |
| Omit unused clears | `s2_zi_asymlaplace` | 5025 / 4714 | 1.064 ± 0.032 |
| Direct small-call binding | `s2_gev` | 6380 / 5839 | 1.097 ± 0.010 |

The other target and ordinary-control measurements in each ablation were
within roughly 1% in either direction. Every measured value was identical.
The final phase probe, comparing both changes with the start of this pass,
measured 4604→4317 ns, 5060→4778 ns and 6553→5589 ns respectively; the
stronger GEV result in that separate packet is not substituted for the
isolated 9% estimate. Raw samples and dispersion remain in the packet.

The complete CLI experiment alternated the before binary, after binary and
independent CmdStan executable, using seeds 1–4, two repetitions per seed,
1,000 warmup and 1,000 retained draws. Stanli source compilation/preparation
and process startup are included; CmdStan's C++ build is excluded. Each
engine ran serially with one thread, without other builds/tests/benchmarks
launched by this task. This remains a shared desktop measurement.

| Model | Before ms ± MAD | After ms ± MAD | CmdStan ms ± MAD | CmdStan / after | Paired ratio median ± MAD |
| --- | ---: | ---: | ---: | ---: | ---: |
| `sw_asymlaplace` | 146.22 ± 6.48 | 131.98 ± 3.64 | 110.60 ± 6.62 | **0.838** | 0.809 ± 0.022 |
| `s2_zi_asymlaplace` | 172.91 ± 6.59 | 170.13 ± 5.72 | 146.79 ± 5.03 | **0.863** | 0.868 ± 0.059 |
| `s2_gev` | 125.58 ± 2.83 | 109.16 ± 4.67 | 91.49 ± 2.58 | **0.838** | 0.806 ± 0.022 |

All three clear the 0.8 target in this focused run. Two paired medians are
close to the boundary, so this is not a guarantee across hosts or runs. The
ZI model's CLI gain is especially noisy: 1.6% from separate wall medians
versus 6.6% from paired speedup medians. Keep the warm-gradient ablation as
the clearer causal evidence. Before/after CSV draws, including sampler
statistics, were identical for all six measured models, four seeds and both
repetitions. The three target models' Stanli and CmdStan draws also match
the previous full sweep, so its clean diagnostic results apply unchanged.
This is not a new full 124-model timing sweep.

Gaussian, Bernoulli and eight-schools CLI medians changed by +0.07%, +0.23%
and −1.0%. Their preparation medians were unchanged or lower, and warm
results across the three measurement packets show no consistent slowdown.
The phase packet's Bernoulli warm median rose 2.4%, within its ~3.4% MAD;
eight-schools' first call rose 1.85 µs with 1.83 µs candidate MAD. These
measurements do not prove a zero-cost bound. Target preparation medians
rose by 0.025–0.068 ms; target first calls changed by −2.1 to +2.7 µs.
The two ordinal controls retained their graph sizes; preparation changed
14.65→14.77 ms and 49.20→47.64 ms, with peak RSS +0.16 and +0.13 MiB.

No persistent metadata or larger tape layout was introduced. Target peak
RSS changes were −32 to +8 KiB in the phase probe, while ordinary small
controls were about 96–104 KiB lower. These small RSS differences can
include page residency/allocator variation; they are not allocation proofs.
The native shared library changed 39,999,088→39,999,232 bytes (**+144 bytes**).
Download, wheel and browser sizes were not remeasured.

Validation of the retained source:

- All 266 configured CTests pass: the initial run passed 265; the remaining
  island cost test needed the two expected counts reduced by one for the
  removed clear, then passed on rerun. Disabling the optimization reproduced
  and passed the old expectations before they were updated.
- Full CmdStan replay: **329/329**, 1,020,194 values. The previously documented
  non-brms cancellation exception and existing gates are unchanged.
- All **124 brms × 3 points** retain their prior results: 367 finite LPs
  (max 2 ULP), 5,079 gradients (max 10 ULP), and 9,928 exact outputs. All
  15,374 compared values and checker output match the before packet exactly.
- The clone/data-mutation regression failed on the old binding and passes
  after the correction. Both pointer mutation and `set_values` detach the
  right contexts while leaving other executor copies unchanged.
- `git diff --check` passes. No compiler overlay changes were made in this pass.

[Follow-up scorecard](2026-09-22-issue-374-followup.csv) records the phase
medians/MAD, numerical maxima and complete sampling results. The separate
[full-sweep scorecard](2026-09-21-issue-374-results.csv) retains its original
measurements. Reproduction artifacts in the new cache packet include
`probe_gradients.py`, `measure_sampling.py`, `measure_phases.py`,
`brms_numerics.py`, raw CSVs, profiles, source and executable hashes,
`followup.patch`, `ctest.log`, `ctest-rerun.log`, and `corpus.log`.
No further tuning is planned in this pass.

### Publication validation

The local inliner patch now includes the argument-order and void-function
handling from [upstream draft stanc3 PR #1726](https://github.com/stan-dev/stanc3/pull/1726),
while retaining the local opt-in setting. Binding a repeated scalar argument
must stay beside that argument's own inlined statements; collecting bindings
after all nested arguments can reorder RNG and print effects. A focused
regression failed before the correction and passes afterward, including a
void function with a repeated RNG argument.

Compiler unit tests and native/JavaScript byte parity pass after the change.
Of the 125 measured models, 122 retain byte-identical compiled MIR, including
all three performance targets. Only `s2_wiener`, `s2_zi_beta`, and
`s2_zoi_beta` change argument-binding order. The final rebuilt native binaries
pass **266/266 CTests in one run** and **329/329 CmdStan corpus replays**
(1,020,194 values), including the existing 10-ULP gate for all 124 brms
fixtures. The 51 verifier unit tests and clang-format 22.1.8 check also pass.
This final correction was not followed by another full timing sweep.

The final local shared library is 39,998,608 bytes: +36,048 bytes (0.09%)
against the saved main-branch baseline. Earlier sizes above describe their
respective measurement packets. Final validation logs and the MIR hash
comparison are under `.cache/issue374-small-perf-20260922/pr/`.
