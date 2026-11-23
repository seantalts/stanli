# Forward-only structured-loop storage

## Scope and evaluator

The synthetic covariance investigation for [issue #656](https://github.com/seantalts/stanli/issues/656)
did not reproduce the reporter's error. It did expose an output-generation
storage problem without requiring their model or data. This change addresses
that confirmed problem, not the unconfirmed error or all preparation costs.

Baseline: fetched `origin/HEAD`, `0eb880b9b9579cd28ca64c3afa4257cba9adca1e`.
The candidate is isolated on `codex/covariance-storage`. Both builds use
Release, the pinned embedded compiler, debug switches off and the density
oracle off. Local toolchain: Apple clang 21.0.0, macOS arm64. The locally built OCaml
dependency declares a macOS 26 minimum while CMake targets 11; its link warning
is retained in the logs. These measurements do not validate deployment to
older macOS versions.

The provisional performance gate is to remove at least half the extra live
memory after output generation at N=256, with unchanged results and no
confirmed regression in affected ordinary workloads. This is an engineering
target for this change, not a user-specified budget. Measure preparation,
first/warm gradients and first/warm output rows separately. Compare live
allocator bytes with RSS: the allocator may retain freed pages.

Correctness requires bitwise agreement with the previous path where its
arithmetic is unchanged, independent CmdStan checks under the existing gates,
and matching names, shapes, exceptions and effect/RNG order. Exercise aliases,
duplicate indices, overlapping RHS, changing branches/extents, zero/one/many
trips, retries, clones and an unrelated reduction/control-flow canary.

## Attribution and alternatives

Fresh local baseline, synthetic UDF covariance N=256: preparation leaves
105.98 MiB live malloc storage; the first output row raises it to 313.39 MiB.
Repeated rows and changed parameters reuse the same allocation. Freeing the
model returns live storage to 5.94 MiB. The output graph contains 193 retained
structured loops. Each loop retains copies/history after its results have
already been copied to the surrounding output graph. This is retained model
state, not cumulative per-row leakage. Baseline first/warm output calls were
about 61/15.4 ms in this attribution run; controlled paired measurements remain
the performance evidence.

Alternatives considered:

- Packing initial slots cannot simply omit imports: the recorder copies
  imports into its initial arena, and those values remain live until writes.
- Releasing tapes after each existing recording would save retained bytes but
  still pay recording, versions, undo and replay construction on every row.
- Enabling `values_only` globally would change selected Stan Math arithmetic,
  including active QR versus double LU solves. That is a separate numerical
  change and is deliberately excluded.
- Changing scalar-loop lowering or shared-adjoint batching could reduce
  preparation, but requires different compiler/adjoint-order proofs. It is not
  necessary to eliminate this output-only history.

The selected mechanism evaluates the existing prepared control tree with one
private value buffer per logical slot and reusable per-call kernel temporary
storage. It avoids history whose reverse consumer cannot exist.

## Semantic proof

`Graph::forward_only` already forbids gradient/reverse. The executor propagates
that fact as `EvalState::no_reverse`, distinct from `values_only`, preserving
the selected active forward arithmetic. Differentiable graphs retain their
existing execution path. Nested kernels receive the same evaluation resources.

Eligibility is deliberately narrower than all forward-only graphs. The
existing plan-collection traversal refuses container-valued loop invariants
and container Alias nodes nested in a For/While. These cases keep retained
execution and its cheap pointer bindings/cross-evaluation constant folding.
Scalar indexed matrix fills remain eligible. No additional preparation walk
is introduced.

Prepared structured loops admit only stateless body kernels, excluding nested
loop/island/region execution and callbacks with `make_state`. Alias lengths
are checked and slot ranges are disjoint. Alias nodes copy values, preserving
snapshots across later assignments. Ordinary kernel outputs and scratch are
staged until the callback finishes; descriptors reset before each invocation.
Absent secondary outputs remain null. Dynamic logical extents use the existing
helper and fixed capacities remain allocated.

Fused indexed updates have an unused output alias removed by the existing
proof. They update their owned base buffer using the existing index enumerator
and staged RHS, retaining duplicate-index order. Copying the complete base
for each scalar write would reintroduce a prohibitive matrix-fill cost and is
avoided. No existing in-place primal-reader safety guard is relaxed.

Control-flow bounds, branch evaluation and break/continue scope follow the
existing walker. Target values accumulate in source order from zero, matching
`reduce_target`. Outputs are copied before per-call storage is destroyed,
including on the next loop invocation. The path keeps no reusable numerical
state across points or executor copies, and an exception releases its local
storage. Pure invariant calls reuse the existing prepared generation proof,
with private per-call snapshots restored after aliases overwrite their output
slots. Cached output cells are bounded by the unique output-slot sizes, not
the trip count. A counted Cholesky test exercises skipped branches, loop
reentry, overwritten outputs and repeated evaluations. Restoration still copies
cached outputs where the recorder could rebind a pointer; output timings must
cover that cost.

## Validation state

Fifteen focused CTests pass, including a separate shipped-configuration entry
for the new control-tree path. The original structured-loop suite depends on
developer switches and is disabled in this configuration; its new focused
entry does not depend on those switches. Tests cover snapshots, duplicate and
overlapping indexed assignments, awkward-length reductions/scratch, target
order, control flow, invariant restoration, failure/retry, independent clones,
RNG consumption and preservation of active QR rather than double LU.

The complete recorded numerical replay passes: 414 models, three points per
model, 1,504,488 values, including outputs, under the unchanged repository
gates and ledger. This is not a universal 10-ULP claim: the existing measured
exception for epinow2 remains (scaled error 3.88e-11, 273251 measured ULP).

[Final exact-source synthetic CmdStan checks](data/2026-11-23-forward-loop-storage/v2-cmdstan-covariance.json.gz)
cover two covariance forms, N=0/1/35/36/128/256, jitter 0.1/1e-14 and three
points each: 24 cases, 72 points. Every candidate density, gradient and full
output vector is bitwise identical to the baseline; names match CmdStan.
Density/gradient agreement with CmdStan is within 1 ULP. Five checksum sums
are 11–13 ULP from CmdStan at larger sizes, identically in both revisions;
no tolerance was widened. Other outputs remain within 10 ULP. These existing
sum differences are outside the aspirational mostly-10-ULP goal and remain
visible rather than being explained away by a scaled-error gate.

Both revisions prepare and dump all 402 benchmark corpus models successfully.
Final selection and performance evidence is recorded below. A final fetch
confirmed that `origin/HEAD` still names the same baseline; no upstream
integration issues occurred.

## Initial paired output-storage result (v1)

Six alternating-order fresh-process pairs for each of seven cells (84 workers)
passed without failures, timeouts or resource guards. All density/gradient and
full-output byte hashes, including changed parameters, agree between revisions
and across trials. Inputs, complete phase observations, source/MIR/library
hashes and commands are retained in
[raw pairs](data/2026-11-23-forward-loop-storage/local-pairs.json.gz),
[summary](data/2026-11-23-forward-loop-storage/local-summary.json.gz) and
[provenance](data/2026-11-23-forward-loop-storage/local-provenance.json.gz).
The evidence index retains its original agent-worktree paths; the adjacent
files preserve the indexed bytes unchanged.

N=256 UDF medians, with observed min/max where useful:

| Measurement | Baseline | Candidate |
| --- | --- | --- |
| Preparation | 325.6 ms (319.8–330.9) | 318.0 ms (308.5–332.6) |
| First output row | 61.62 ms (58.98–64.64) | 13.35 ms (13.07–15.47) |
| Second output row | 18.04 ms (15.09–20.86) | 13.30 ms (13.02–14.94) |
| Extra live malloc bytes, first row minus preparation | 207.414 MiB | 1.031 MiB |
| Peak process RSS after first row | 367.03 MiB (362.45–370.02) | 180.67 MiB (178.31–184.84) |

The candidate's extra live allocation includes the 1.004 MiB output row
buffer. Peak RSS is about 51% lower; the provisional memory gate passes.
Preparation live-byte medians vary between processes (103.6 versus 114.5 MiB),
so these results do not establish a preparation-memory improvement. The
remaining preparation cost and scalar/native program size are unresolved.

N=128 UDF first/second rows improve from 1.565/0.704 ms to 0.758/0.644 ms.
UDF N=1, built-in covariance N=1/128/256 and the unrelated normal-100 control
retain the same output live-byte counts; their sub-millisecond timings remain
noisy. The normal-100 first row is 0.006 ms in both revisions. These controls
do not prove a universal speed bound for all structured loops.

Reproduce the paired measurement with the two shipped-configuration libraries:

```sh
python3 harnesses/diagnostics/covariance_heap_pair.py \
  --base /absolute/base/build/libstanli.dylib \
  --candidate /absolute/candidate/build/libstanli.dylib \
  --pairs 6 --output /absolute/output/paired-heap
```

## Counterproposal and narrowed candidate (v2)

The initial candidate passed numerical checks but was not accepted. The
standalone [invariant probe](../../harnesses/diagnostics/forward_loop_invariant_probe.cpp)
bypasses source-compiler folding to expose admitted runtime control trees.
At matrix N=128 and 1000 trips, the initial bounded path took 7.72 ms warm
without overwrites and 9.31 ms with a matrix reset, versus about 0.0045 ms for
retained replay. Repeated whole-matrix snapshot restoration/assignment was a
confirmed regression, not an acceptable memory tradeoff.

V2 adds the structural eligibility guard above and retains the original fast
path for those cases. Initial numerical, selection/preparation, paired heap
and invariant-probe evidence remains preserved rather than overwritten.
The [initial probe](data/2026-11-23-forward-loop-storage/v1-invariant-probe.jsonl.gz)
and [final probe](data/2026-11-23-forward-loop-storage/v2-invariant-probe.jsonl.gz)
retain all samples. All eight final cases are correct; warm N=128/1000 calls
are about 0.0049 ms in either path, eliminating the measured regression.

The final 15 focused tests, 414-model recorded replay and 72 independent
synthetic CmdStan points all pass again. All 72 covariance density/gradient
and output byte comparisons with baseline are exact, with the unchanged
CmdStan checksum limits described above. The runtime source files used by the
frozen tools are identified in [this SHA-256 manifest](data/2026-11-23-forward-loop-storage/v2-runtime-source-sha256.json).
The tools were built before the task commit, so their build IDs retain the
baseline SHA; artifact hashes and this source manifest identify the candidate.

## Final paired storage result (v2)

The repeated 42 pairs / 84 workers pass all exact hashes without failures,
timeouts or resource guards. See [raw pairs](data/2026-11-23-forward-loop-storage/v2-local-pairs.json.gz),
[summary](data/2026-11-23-forward-loop-storage/v2-local-summary.json.gz) and
[provenance](data/2026-11-23-forward-loop-storage/v2-local-provenance.json.gz).
N=256 UDF results, medians with observed ranges:

| Measurement | Baseline | Final candidate |
| --- | --- | --- |
| Preparation | 317.56 ms (312.59–331.71) | 320.15 ms (307.09–334.09) |
| First output row | 59.51 ms (58.80–64.65) | 13.38 ms (13.01–15.23) |
| Second output row | 16.17 ms (15.52–19.97) | 13.25 ms (13.02–14.79) |
| Extra live malloc bytes, first row minus preparation | 207.414 MiB | 1.032 MiB |
| Peak process RSS after first row | 366.66 MiB (362.52–367.83) | 181.02 MiB (177.59–184.33) |

The memory gate passes. The guard keeps the matrix-fill benefit while retaining
the fast path for the measured adverse invariant cases. N=128 first output is
1.494 versus 0.770 ms; warm 0.629 versus 0.642 ms has overlapping ranges.
Small/control live allocations remain effectively unchanged. Substantial
preparation storage/time and the original reporter's error remain unresolved.

## Corpus performance and remaining limits

[Evaluator provenance](data/2026-11-23-forward-loop-storage/v2-evaluator-provenance.json)
indexes exact commands, source/tool/binary identities, full replay logs,
selection, all preparation subprocess observations and benchmark records.
Large raw program/benchmark archives remain preserved at the indexed local
paths with hashes; the compact records do not replace those raw observations.
V1 evidence remains separately indexed as superseded, including the rejected
invariant case and the initial strict-toolchain preflight failure. Restoring
the missing private pinned-compiler symlinks made the strict guard pass in
both arms; no pin override or shared dependency mutation was used.

The [final selection](data/2026-11-23-forward-loop-storage/v2-corpus-selection.json)
compares all 402 models: zero changed programs, nine `LOOP` users, eleven
controls, four preparation flags and zero selection failures. Every model
received three fresh preparation measurements per arm, with alternating arms
and retained raw results, using the selector's best-of-three statistic. The named edited kernel is `OP_LOOP`; no other
kernel consumes the new `no_reverse` field.

The nine opcode users are `cm_invgaussian`, `covid19imperial_v2`,
`covid19imperial_v3`, `ctsem_ctsm`, `ctsem_ctsm_400`,
`ctsem_ctsm_post4_100`, `iohmm_reg`, `mlumr_survival_mspline_relaxed` and
`mlumr_survival_relaxed`. Controls are `i319_pois_re`, `sw_ar`,
`eight_schools_noncentered`, `hmm_example`, `one_comp_mm_elim_abs`,
`gpcm_latent_reg_irt`, `cm_lba2`, `mlumr_survival_mspline_relaxed`,
`hierarchical_gp`, `kronecker_gp` and `oncobayes2_blrm_exnex_combo3`.

Preparation flags were `examplemodels_ms` (372.2 to 387.1 ms),
`hbayesdm_gng_m4` (162.8 to 172.8 ms), `hmm_gaussian` (109.2 to
115.4 ms) and `ldaK5` (423.6 to 438.2 ms). Their union with opcode users
and controls selects 23 models. The earlier V1 adverse `mlumr` signal also
remains covered. Aggregate preparation changes by about +0.49%; this does
not erase the individual flags.

Both arms use the unchanged corpus protocol: six paired engine rounds,
10 committed points, 20 ms warmup and 40 ms measurement, matching hardware,
compiler and shipped configuration, with strict pinned CmdStan provenance.
All reference builds settled before timing; arms ran sequentially without
other measurements. [Baseline TSV](data/2026-11-23-forward-loop-storage/v2-base-corpus-benchmark.tsv),
[candidate TSV](data/2026-11-23-forward-loop-storage/v2-candidate-corpus-benchmark.tsv)
and the [per-model phase/total comparison](data/2026-11-23-forward-loop-storage/v2-comparison.json)
retain all 23 rows. Twenty-two complete in both arms. No measured total-time
regression exceeds max(2%, 5 ms): complete-model deltas range from -1.948%
to +1.805%. These are finite measurements, not a universal zero-overhead claim.

Six alternating fresh preparation pairs investigate the flags rather than
discarding them. [Follow-up observations](data/2026-11-23-forward-loop-storage/v2-preparation-followup-events.json)
and [summary](data/2026-11-23-forward-loop-storage/v2-preparation-followup-summary.json)
retain a real-looking `examplemodels_ms` signal: paired median +12.86 ms
(+3.37%, all six positive, MAD 6.82 ms). Its total estimate increases
0.877%, below its 2.017 s regression threshold. This preparation cost remains
an explicit tradeoff, not a claim of no penalty. `ldaK5` is +4.91 ms with
mixed signs; the other initial signals reverse or remain noisy. Small controls
show no preparation penalty in this follow-up.

The incomplete case is the already published `ctsem_ctsm_post4_100` point-4
failure: [the benchmark guide](../../docs/benchmarks.md#numerical-gate) and
[published result](../../output/corpus-performance/README.md) already record
scaled error about 1.38e-9. Current q[449] is -1.013339807475468; its gradient
is 1.5153796300399438 versus CmdStan 1.5153796279544542, absolute difference
2.0854896e-9. No tolerance was relaxed or model dropped. [Exact point and full
vectors](data/2026-11-23-forward-loop-storage/v2-ctsem-point-diagnostic.json.gz)
show baseline/candidate densities and every gradient bitwise identical at all
ten benchmark points. [Fresh/reordered checks](data/2026-11-23-forward-loop-storage/v2-ctsem-sequence-discriminator.json.gz)
also agree for fresh point 4, 0→1→2→3→4 and 4→0→4, including CmdStan.
The cause remains unknown; the existing ledger reports an unexplained bound
and an 80-digit timeout. This is an unresolved benchmark case, distinct from
the passing three-point recorded numerical replay, and is not explained away
as cancellation or stale state.

The [native size record](data/2026-11-23-forward-loop-storage/v2-native-binary-sizes.json)
is 45,499,872 to 45,500,672 bytes for the native build artifact (+800 bytes),
and 13,721,356 to 13,725,785 bytes with gzip level 9 (+4,429 bytes). This is
a build-artifact comparison, not a rebuilt wheel/browser package. No runtime dependency was
added. Compiler/test/format logs are retained beside the evidence, including
the local OCaml dependency link diagnostic noted above. The affected runtime
objects were forced to recompile and complete compiler output was inspected;
no project C++ diagnostics remain. Formatting uses clang-format 22.1.8.
