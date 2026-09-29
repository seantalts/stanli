# Native kernel profile and next evaluator

Baseline c3d0f929, Release Apple Clang 21 arm64. Instruction generation and
dispatch JIT remain tabled. Seventeen shared-corpus model/data cases were
compiled through the shipped embedded compiler, then measured at bench_grad's
deterministic point. Existing opcode profiling uses 50 measured gradients after
warmup. Separate uninstrumented runs use 200 ms warmup / 250 ms measurement.
Opcode times include instrumentation and nested-region overlap; they are
attribution clues, not uninstrumented throughput estimates.

The IRT cases spend about 93% in the already recorded categorical-logit GLM;
softmax is only 0.2% in gpcm. Survey spends 95% in binomial. HMM cases primarily
show indexing, arithmetic and updates. Removing their tiny softmax tapes cannot
deliver a useful whole-gradient improvement. Do not choose by name count.

The expanded profile identifies normal_id_glm: 55% of instrumented small
sw_gaussian and 99% of diamonds. The existing kernel builds scalar var inputs,
calls Stan's analytical partials implementation, runs a unit reverse, copies
partials into scratch and discards the tape. Its backward already contracts
the stored partials. Kronecker GP also spends 21% in the Cholesky-correlation
constraint; retain that as a separate next candidate.

## Normal identity GLM proof packet

Claim: route the same unmodified Stan Math prim/prob function through the
existing rvar partial recorder to eliminate var allocations and unit reverse.
Keep exact activity types for y/X and the established always-active
alpha/beta/sigma types; keep scalar/vector and matrix/row-vector dispatch.
Preserve scratch layout, backward scaling and propto behavior. Handle early
returns through record_probability_call, including explicit zero partials.

Prediction: the small Gaussian model benefits most proportionally. Diamonds'
matrix arithmetic may dominate even though the enclosing kernel dominates
its profile, so 99% kernel share is not 99% removable tape overhead.

Alternative: retain factors or fuse more graph operations. This kernel already
is a fused likelihood with retained partials; neither would remove its
identified per-call tape bookkeeping as directly. A handwritten derivative
duplicates Stan's algorithm unnecessarily. The recorder is the smallest
experiment, adds no new engine/dependency and is independently ablatable.

Semantic evaluator: directly compare kernel values and stored/scattered
weighted partials to the previous Stan var implementation across y/X activity,
scalar/vector outcomes/intercepts/scales, matrix/row broadcast, zero rows and
columns, invalid scales and nonfinite inputs. Prefer bitwise parity; investigate
any mismatch before accepting a tolerance. Check the actual tape allocation
counter, not just a registry label. Run focused lowering/GLM/adjoint tests, full
native tests and the recorded independent CmdStan corpus before integration.

Performance evaluator: saved baseline library and bench_grad in
.cache/native-kernel-baseline; six alternating fresh-process pairs on diamonds,
sw_gaussian and unrelated eight-schools canaries. Measure preparation, first
and warm gradients, inference, peak memory and binary size separately. Initial
selection gate remains >=10% warm-gradient improvement on a real target with
no resolved ordinary-canary regression. Do not claim broad throughput from a
kernel microbenchmark. If the direct recorder fails parity, fix its type/layout
contract or retain the old path; never substitute a model-specific rule.

Profile manifest, input hashes and all raw instrumented/uninstrumented outputs
are retained in the adjacent native-kernel-profile.json artifact. Profiling is
bounded to these cases; this is not an exhaustive weighted workload census.

## Correctness checkpoint

The direct-recorder candidate passes exact internal comparison across 0/1/5/32
rows, 0/1/3 columns, eight activity masks, scalar/vector y/alpha/sigma and
matrix/row-vector X, full/propto, weighted/zero/infinite seeds, nonfinite inputs
and invalid scales. A 2048x8 active-design probe does not grow Stan's arena.
This observes removal of the previous large allocation; unchanged arena capacity
alone is not a general proof that no small allocation could occur. Source review
confirms the replacement contains no var/nested tape, and registry metadata now
identifies recorded_partials. Values-only/reuse and prior adjoint accumulation
are tested separately. Signed-zero normalization preserves the previous unit
reverse's initial positive-zero accumulation.

All 278 native tests pass. All 329 recorded CmdStan models pass at three points,
1,020,194 values, with all 124 model ULP gates passing. Worst scaled discrepancy
remains 9.38e-13 / 7040 ULP; this does not imply a universal ten-ULP bound.
Performance measurements are the next adoption gate, not inferred from tests.

## Bounded canary confirmation, declared before follow-up results

Initial public-API measurements show +9.5% short noncentered eight-schools
inference time, despite essentially flat warm gradients; centered inference is
+5.1% with broad dispersion, warm gradients +3.3%. Do not dismiss these signals.
Run six alternating native bench_grad pairs on both targets/canaries, then
exactly eight A/A and eight A/B pairs per canary using repeated warmed 100+100
sampling calls for a 250 ms measurement window. Verify identical draw hashes.
Report paired ratios/dispersion. A persistent A/B shift outside A/A dispersion
is a regression requiring resolution; overlapping signals remain inconclusive,
not proof of no slowdown. No open-ended reruns to choose a preferred result.
