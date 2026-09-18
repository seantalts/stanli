# Native reduce_sum feasibility experiment

Current authorization: begin the reviewed design's bounded prototype. Baseline
is `be0a0c8d4882afca3e1d1de0083267df10f8fb3d`. Production runtime behavior remains
unchanged. The two design/review documents were already untracked at entry.

Current checkout was refreshed to `origin/main` at `504e8d80` on user request;
this document's stage-one measurements describe the old `be0a0c8d` base.

Status: stage one complete; stage two implements compact child imports and
exclusive gradient publication. See [the current stage-two record](2026-09-18-native-reduce-sum-imports.md).
The large cases support native child graphs; small reductions still need serial
execution. No public threading option or runtime lowering has been added.

## Evaluator fixed before implementation

Use a standalone C++ harness and a Stan fixture with a pure callback. Its data
selects either ordinary reduce_sum or a fixed-bound direct callback call. Each
chunk is compiled independently through existing model lowering; repeated
shared arguments therefore retain existing binding/alias semantics. This avoids
a new production opcode or a dependency on private Lowering internals. It also
retains full parameter declarations per child, a conservative prototype memory
and packing cost to report rather than conceal.

Compare whole-slice execution, identical four-chunk serial execution, and that
same plan on persistent workers, counting owner participation in the thread
limit. Include all per-gradient input copies, dispatch, callbacks, and merging.
Measure first-use and warm evaluations separately. Record construction time,
graph/arena sizes, process peak RSS, and allocation diagnostics. No strict
allocation-free gate. Use ten counterbalanced samples and report median/IQR.

Require bitwise serial-chunk/parallel equality at multiple deterministic points.
Compare whole-slice and independent analytic log-density/gradient oracles using
the design's provisional `1e-10 + 1e-10*abs(reference)` tolerance. Exercise normal
and Student-t likelihoods, active sliced values, repeated shared aliases, empty
and uneven chunks, worker exception/recovery, and multiple thread counts.
An unchanged ordinary fixture and existing reduce_sum tests are canaries.

Hypothesis: expensive callback arithmetic outweighs persistent dispatch and
shared-gradient merge, giving useful within-chain speedup without runtime
integration. Competing explanation: short vectorized kernels are already so
cheap that partitioning and dispatch dominate. A whole-slice versus serial-chunk
comparison distinguishes partition costs from scheduler costs. Shared-parameter
width and active-slice sweeps expose copying/merge limits.

Counterproposal: thread individual density kernels and avoid child binding.
Defer that implementation until phase measurements show child overhead is the
limiting cost; this experiment measures its removable headroom. Reusable
register callbacks remain untested and become relevant if preparation or
retained memory, rather than warm arithmetic, limits the chosen representation.

Stopping artifact: correctness results, reproducible timing/memory evidence, and
a decision on proceeding to retained-op integration. The provisional 2x at four
workers target is useful evidence, not a user-mandated threshold. No claim about
arbitrary callbacks, general incoming adjoints, CmdStan scheduling parity, or
complete inference follows from this unit-seed experiment.

## Results, 2026-09-18

Apple M3 Ultra (32 logical CPUs), macOS 26.6.2, Apple clang 21, Release
`-O3 -DNDEBUG`, `-ffp-contract=off`, `STANLI_THREADS=ON`. Clean local build.
[Raw evidence](../../native-reduce-sum-probe.json) retains source/binary/MIR hashes,
dependency identities, all samples, correctness cases, and an opcode profile.
No builds or other work from this task ran concurrently with the final timings.

Dependencies were reused from another checkout at the pinned revisions. That
Math checkout has an existing one-line CVODES adjoint fix (zeroing qBdot), dated
September 11, before both builds. The exact diff is recorded in the evidence;
these callbacks do not execute ODEs. The external checkout was not modified.
The runner now records dependency diffs as well as commit IDs. This metadata-only
addition postdates the timing run; the recorded measurement runner hash names
the version actually used, and the evidence also records the updated runner hash.

Each row is a fresh process, three correctness parameter points, warmup, and ten
counterbalanced samples. Medians are microseconds per complete gradient,
including parameter copies, dispatch and merge. Parentheses give parallel IQR.
Four fixed chunks are used; the calling thread counts toward the thread limit.
There are three shared parameters except in the last row.

| N | Callback / inputs | Threads | Whole slice | Serial chunks | Parallel (IQR) | Speedup vs whole |
|--:|:--|--:|--:|--:|--:|--:|
| 1,000 | Normal, data slice | 4 | 5.72 | 5.66 | 14.42 (0.45) | 0.40x |
| 1,000 | Student-t, data slice | 4 | 9.46 | 9.24 | 12.41 (0.26) | 0.76x |
| 10,000 | Normal, data slice | 4 | 58.03 | 56.14 | 27.22 (2.20) | 2.13x |
| 10,000 | Student-t, data slice | 4 | 93.27 | 88.43 | 35.87 (2.85) | 2.60x |
| 100,000 | Normal, data slice | 4 | 558.46 | 531.89 | 163.29 (2.86) | 3.42x |
| 100,000 | Student-t, data slice | 4 | 920.99 | 883.54 | 245.55 (1.86) | 3.75x |
| 100,000 | Student-t, data slice | 2 | 940.47 | 891.54 | 456.17 (7.63) | 2.06x |
| 100,000 | Student-t, active slice | 4 | 960.34 | 1148.65 | 443.00 (3.26) | 2.17x |
| 10,000 | Student-t, 4096 shared parameters | 4 | 99.20 | 108.36 | 49.21 (3.02) | 2.02x |

The large Student-t scheduling speedup versus the *same partition* is 3.60x.
Its 3.75x versus whole slice also includes a modest benefit from smaller serial
vector operations. The full matrix includes one-worker and additional size /
shared-width cases. Small-case regressions are retained, not excluded.

All serial-chunk/parallel results are bitwise identical. Whole-slice and
independent analytic comparisons pass the predeclared tolerance. The timing
matrix's largest absolute difference is `1.834e-9`; the maximum fraction of
allowed tolerance is `0.000222`. Repeating `b[1]` as another callback formal
checks alias-gradient accumulation, alongside active slices and absolute bounds.

## Memory and limiting costs

For large data-slice Student-t, the three main executor arenas (values,
adjoints, scratch) total 8.39 MiB whole-slice versus 15.26 MiB across four children.
With active slices they total 9.16 versus 17.55 MiB. These exclude graph metadata,
result vectors and opaque kernel state. Process peak RSS was 47.4 and 50.6 MiB
respectively, including preparation and both comparison implementations.

The prototype retains full parameter declarations and some full-size data per
child, a conservative cost to remove through real callback import bindings.
Its owning Executors are allocated before the gradient loop, and no whole-arena
merge occurs. Each timing case's diagnostic warm evaluation reported zero
ordinary C++ new/new[] calls. This excludes aligned allocation, direct malloc and
arena cursor movement; zero allocations is neither a general claim nor a gate.

Large data-slice Student-t phase medians were 0.04 us packing, 245.75 us dispatch
plus child gradients, and 0.08 us merging. With active slices these became 44.42,
353.02 and 79.85 us. These separately instrumented medians need not add up to the
uninstrumented total. Full child adjoint clearing is another active-slice cost.
Smaller imports and disjoint derivative writes are therefore worth testing.

The separate opcode profile attributes about 63% of child kernel time to
Student-t and 37% to the mean expression's arithmetic, predominantly reverse
reductions. Threading just the density kernel would leave that other work
serial. Negligible packing/merge for small shared vectors also argues against
changing that boundary now. Reusable register callbacks remain untested:
preparation was 2.31 ms whole versus 5.02 ms for all children in this case,
which is not the limiting phase for repeated gradients. Team setup was 0.042 ms,
and first parallel evaluation 294 us versus 246 us warm.

## Verification and limits

- 49 configurations pass for N=0/1/7/31, both densities, data/active slices and
  1/2/4 workers, plus an uneven 8-chunk/8-worker case. Each checks three parameter
  points, repeated bitwise results, an executor clone, and worker domain-error
  recovery where N is nonzero.
- A scheduler check injects two different errors, verifies lowest-chunk error
  selection after every task drains, then reuses the team successfully.
- The same 49 configurations pass under ThreadSanitizer. An additional 9,000
  timed evaluations (3 modes x 3 samples x 1000 iterations), including 3,000
  parallel evaluations, produce no sanitizer diagnostics.
- Renaming the callback and its alias formal passes analytic and bitwise checks.
  Existing test_reduce_sum and test_multichain pass. An unchanged AR1 canary
  gives the same printed log-density checksum and parameter count before/after; its single timing
  samples are sanity checks, not a regression study.
- C++ formatting, Python syntax, and whitespace checks pass.

This is a developer-only unit-seed feasibility harness. It implements no
automatic eligibility/fallback or production parallel boundary. Arbitrary
incoming adjoints, storage-view binding, public APIs, cancellation, real brms
workloads, external CmdStan comparison, Windows/Linux validation and complete
inference remain integration gates. No runtime source or ABI changed. The full
configured test suite was not run for this isolated target.

## Reproduce

Run from the repository root with pinned dependencies available. A fresh machine
can prepare them with `tools/dev_setup.sh --no-build`. Omit ccache settings if
ccache is unavailable.

```sh
cmake -S . -B build-reduce-sum -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DSTANLI_THREADS=ON
cmake --build build-reduce-sum --target bench_reduce_sum test_reduce_sum test_multichain -j 8
python3 tools/bench_reduce_sum.py --output build-reduce-sum/evidence/final
ctest --test-dir build-reduce-sum -R '^(test_reduce_sum|test_multichain)$' --output-on-failure
build-reduce-sum/bench_reduce_sum --n 100000 --kind 1 --check-only --profile
cmake -S . -B build-reduce-sum-tsan -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DSTANLI_THREADS=ON -DSTANLI_SANITIZE=thread
cmake --build build-reduce-sum-tsan --target bench_reduce_sum -j 8
python3 tools/bench_reduce_sum.py --binary build-reduce-sum-tsan/bench_reduce_sum --checks-only --output build-reduce-sum/evidence/tsan
build-reduce-sum-tsan/bench_reduce_sum --n 31 --active 1 --iterations 1000 --samples 3
```

The executable is excluded from normal builds and is not installed. The runner
retains raw JSONL, identities, and a summary; each timing case is a separate
process. Source compilation to MIR is outside the preparation timing.

## Next bounded experiment

Keep the fixture and partition, but replace full-parameter child models with
callback import bindings and scatter maps. Reuse retained-loop alias/import
rules; reserve state once, share immutable inputs where safe, and write disjoint
active-slice derivatives directly to owned ranges. Compare parity, time and
retained bytes against this harness before committing to a production storage
interface. Then integrate the retained reduction and a small-work serial policy.
Keep arbitrary-seed reverse separate from the proven unit-seed experiment.


## Stage two evaluator (fixed before implementation)

Authorization: continue implementation and the import/storage experiment. Preserve
stage one's harness and executable under `build-reduce-sum/stage2-baseline/`.
A fresh active N=100,000 baseline is recorded there before edits.

Hypothesis: contiguous import ranges, coalesced by source slot, remove most
full-parent copying/clearing and gradient merging. First test a conservative
probe-only graph rewrite: retain the convex hull of statically indexed reads
from unwritten input slots, keep full inputs for all other consumers, and
preserve every operation and its order. Remove storage for unreferenced leaves.
Refuse opaque, dynamic, or stateful graphs transactionally. This reuses the
retained-loop import identity rule (one import per parent slot), without exposing
private lowering APIs. It is an import-planning experiment, not callback lowering.

Use the same process and worker team to counterbalance whole, full-child serial /
parallel, and compact-child serial / parallel execution. Require bitwise parity
against the original children. Prove gradient destination ownership once from
import maps: workers may publish exclusive spans directly; overlapping spans
merge in canonical chunk order. Preserve parameter activity and kernel variants.
Adversarial checks cover overlapping aliases, a whole-input consumer, dynamic /
opaque refusal, repeated execution, and exceptions. Run the existing correctness
matrix, ThreadSanitizer, profiles, and relevant regression tests.

Counterproposal: bind every input by view into a single parent arena and bypass
copies altogether. This would require a new Executor binding interface. The
cheaper compaction experiment first measures remaining copy/clear cost and the
memory lower bound it could improve. Keep views untested until that measurement;
do not infer a zero-copy win from capacity reduction alone.

Continuation gate: lower retained bytes and packing/merge costs with exact
same-partition results, and no material throughput regression at large active
N. No user performance threshold or zero-allocation requirement is introduced.
