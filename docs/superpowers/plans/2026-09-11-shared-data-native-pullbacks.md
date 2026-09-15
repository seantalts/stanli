# Shared executor data, native matrix pullbacks, and streaming JSON

## Scope and baseline

The user authorized all three changes, prioritizing immutable executor data.
Work starts from `3b743045c33e475683b0bb825c7549ab83fd3d06` in an isolated
checkout; the dirty structured-loop research checkout is preserved.
History search found sharing of graph integer payloads and executor pooling,
but no implementation sharing unwritten numeric value slots between executors.

The evaluator is Release on this host, pinned Stan Math/Stan/stanc, with
baseline and candidate built separately. Retain exact log-density behavior;
Cholesky must agree bitwise with its existing varmat backward. GP derivatives
may change summation order: focused checks use 2e-12*(1+abs(reference)), with
full-model comparison against the baseline and the existing CmdStan oracle.
Sharing must preserve cloned values/gradients, repeated evaluations, escaped
writable-pointer independence, source destruction, zero-width slots, secondary
outputs, generated quantities, and concurrent chains. JSON must retain valid
integer/real types, dimensions, flattening and nonfinite spellings, and reject
malformed/ragged input consistently with Stan's reader.

Initial baseline: GP per-op profile reports covariance backward 3.22 us and
Cholesky backward 1.16 us per call. MNIST preparation: 3.352 s, including
2.269 s JSON parsing, and 5,074,108,416 bytes maximum RSS. These are diagnostic
single runs, not speedup claims. Initial full build succeeded; compiler-driven
CTest cases require the embedded compiler missing from the baseline's initial
configuration. The candidate enables the matching pinned embedded object.

## Decisions and alternatives

1. Numeric data: partition unwritten, nonparameter slots from private values.
   Share their buffer across copies; detach on writes. Any exposed writable
   pointer permanently disables sharing from that executor, so later writes
   through a retained pointer cannot corrupt a clone. Compiled fills use a
   copying setter rather than exposing a pointer. Kernel input contexts are
   rebound only on detachment; no copy-on-write check enters gradient dispatch.
   Output and secondary-output writes exclude slots regardless of activity.
   Alternative: immutable compiled-model-owned constants and a separate mutable
   executor API. That could also remove compiler/executor fill copies, but needs
   a wider lifetime/API migration. Defer until clone memory savings are measured;
   this change deliberately addresses per-executor duplication first.
2. Cholesky: call the pinned Stan Math blocked/unblocked pullbacks through tiny
   adjoint views over the saved factor. This removes nested tapes and redundant
   factorization without maintaining another derivative algorithm.
3. GP: native fixed-location exp-quad pair derivatives, reusing forward covariance values.
   Preserve replay for active locations, other covariance families and extreme numeric geometry.
   Add active-location, mixed-activity, repeated-point, asymmetric-seed and
   accumulated-adjoint oracles. The other families are not silently widened.
4. JSON: use Stan's streaming json_data_handler and matching RapidJSON flags.
   Fetch the pinned Stan repository's RapidJSON headers in the existing dependency
   setup. Alternative: an independent SAX handler over bundled nlohmann JSON
   avoids that dependency but retains custom shape and token semantics. Prefer
   deleting those semantics in favor of the same parser CmdStan uses.

Raw local measurements and build logs: `/tmp/stanli-data-kernels-evidence/`
and `/tmp/stanli-data-kernels-build-*.log`. Matched repeated timing, targeted
sanitizers, full configured CTest, corpus checks and final deltas are pending.

## Correctness findings

The first active-location GP prototype changed one existing exact gradient by
four ULP. Rather than weaken that test, the native predicate now requires an
inactive coordinate input. Nonfinite/underflowed intermediates refuse before
publishing local partials. The other families remain unchanged.

The full Release suite passed 241/241 and the reference sweep passed 254/254
models at three points (976,394 values), using its existing documented model
exceptions. A separate baseline/candidate comparison found all MNIST values
and 79,411 gradient entries bitwise equal at all three points, as well as all
three unrelated controls. The two GP models differed by at most 2.27e-16 in
abs(error)/(1+abs(baseline)) before the final extreme-value guard refinement.

The added empty GP test exposed an undefined null reference in Stan Math's
empty Matrix<var> sum oracle; it now uses the exact empty-result oracle instead.
A second UBSan diagnostic comes from Eigen's zero-column product inside the
pinned Stan Math blocked Cholesky pullback. A standalone 36x36 Stan Math
varmat Cholesky + sum + grad reproduces it without Stanli. Copying the saved
factor to an owning matrix did not avoid it and was discarded. Keep the direct
saved-factor view. Default Release/ASan-only tests exercise sizes 0, 1, 5, 35,
36 and 80; the explicit `--unblocked-only` local diagnostic lets UBSan cover
all GP cases and the unblocked native Cholesky path without claiming coverage
of that known failing dependency path. Do not describe the full UBSan matrix
test as passing. Sharing, JSON and multichain UBSan checks are independent.

Retention audit: `inplace.cpp`'s value-free whitelist protects the output of
both kernels; `adjoint.cpp` checkpoints CALL outputs as well as inputs; active
structured kernel calls retain output frames. Reusing forward matrices does
not require widening these existing retention contracts.

## JSON refinement

The first reader used `json_data(istream)` and `from_var_context`. Five matched
runs showed a memory win but a time loss: median MNIST preparation 3.266 ->
3.396 s; parsing 2.203 -> 2.280 s; peak RSS 5.065 -> 4.037 GB. Do not present
that prototype as a preparation speedup. Raw observations are retained separately
as `measurements-initial-json.json` in the local evidence directory.

The final implementation feeds the same Stan handler with RapidJSON MemoryStream
or FileReadStream, retaining full-precision, UTF-8 and nonfinite parsing flags.
It moves completed arrays through DataMap's setters, avoiding both the full text
copy and var_context's by-value array accessors. The final JSON translation unit
and test_data were compiled together under ASan+UBSan and passed. Full Release
CTest and the 254-model reference sweep also passed again after this change.

## Final results and disposition

Implementation commits: `3d9dda2b` (sharing), `f84233d9` (JSON), `c12c1389`
(pullbacks). All changes are enabled. Runtime implementation is net 58 lines
smaller; this excludes added tests, the clone benchmark, and documentation.
[Retained results](2026-09-11-shared-data-native-pullbacks.results.json) contain
all raw measurements, min/max/MAD, input hashes, commands, profiles, the
rejected JSON prototype, verification output and the independent UBSan repro.

Apple M3 Ultra, arm64, Release, Apple Clang 21, `-ffp-contract=off`. Baseline
and candidate run in alternating order as fresh processes, after builds and
correctness checks finish. Gradient measurements warm for 200 ms per process.
Seven gradient repetitions, three clone repetitions, five preparation
repetitions. The seven additional executors in each real-model clone run all
produce the same log density and 79,411 gradient doubles as their prototype,
bitwise. GB below means decimal GB, not GiB.

| Measurement | Baseline median | Final median | Change |
|---|---:|---:|---:|
| `gp_regr` gradient | 6.721 us | 2.600 us | 2.59x faster |
| `gp_pois_regr` gradient | 6.250 us | 2.215 us | 2.82x faster |
| MNIST JSON parsing | 2.227 s | 1.728 s | -22.4% |
| MNIST complete preparation | 3.288 s | 2.835 s | -13.8% |
| MNIST preparation peak RSS | 5.070 GB | 3.832 GB | -24.4% |
| MNIST seven additional executor copies | 416.37 ms | 209.99 ms | 1.98x faster |
| MNIST eight-executor peak RSS, including evaluations | 10.313 GB | 4.802 GB | -53.4% median |
| 64 MB immutable-input fixture, eight-executor peak RSS | 514.9 MB | 130.6 MB | -74.6% |

The real multi-executor candidate RSS ranges from 4.800 to 6.403 GB across
three processes, so report that range alongside its median. Those process
peaks combine parsing and sharing benefits; do not attribute all of the
reduction to sharing. The synthetic fixture isolates sharing with no JSON.
For MNIST the compiled-data buffer is about 378 MB; sharing seven additional
copies avoids about 2.65 GB of explicit data allocations before allocator and
lifetime effects. Per-executor operation contexts, parameters, intermediate
values, adjoints and kernel state remain private.

Phase tradeoffs remain visible: single-executor construction rises from
43.25 to 50.93 ms, and the compile phase from 443.88 to 490.53 ms in the final
preparation runs. The 499 ms parsing reduction more than covers those costs.
No claim is made that every preparation phase got faster.

The unrelated gradient controls have median candidate/baseline changes of
+1.58% for Eight Schools, -0.18% for normal_mixture, and -0.72% for arK.
Identical-candidate A/A ratios span 0.962–1.017 for Eight Schools and
0.940–1.013 for arK over seven pairs. Treat these small signals as inconclusive,
not as established speedups or proof of zero regression. The earlier matched
run had Eight Schools -0.22% and arK +1.81%, reinforcing that distinction.

A diagnostic 10,000-call final profile puts GP covariance backward at 0.132 us
and Cholesky backward at 0.439 us per call, versus 3.223 and 1.160 us in the
initial baseline profile. These instrumented per-op costs are explanatory;
the table's gradient comparisons use profiling disabled.

Final gates: 241/241 Release CTest tests pass; the 254-model CmdStan reference
sweep passes at all three points with existing exception policies (976,394
values, ordinary worst relative error 9.38e-13). Final direct GP comparisons
against the baseline differ by at most 3.44e-16 in
abs(error)/(1+abs(baseline)); active-coordinate exact tests remain unchanged.
Sharing, JSON and multichain pass ASan+UBSan; the native GP and unblocked
Cholesky test passes its explicit UBSan diagnostic. The full blocked Cholesky
UBSan test still reproduces the pinned upstream Eigen issue described above;
Release tests compare both Cholesky algorithms bitwise with the old oracle.

Commands used for the final correctness gates:

```sh
ctest --test-dir build-candidate --output-on-failure -j16
python3 tools/verify_refs.py deps/posteriordb \
  --check build-candidate/stanli_check --jobs 8 --timeout 120
ctest --test-dir build-sanitize \
  -R '^(test_executor|test_data|test_multichain)$' --output-on-failure -j3
build-sanitize/test_native_matrix_pullbacks --unblocked-only
```

The final JSON translation unit and `test_data.cpp` were also compiled and
linked directly under ASan+UBSan; the retained reproduction script and clean
output identify that final reader rather than the earlier prototype. No user
changes in the original structured-loop checkout were modified, and no merge
or publication was performed.

## Published benchmark row refresh

At `de0bd757`, refreshed `gp_regr`, `gp_pois_regr`, and `hierarchical_gp`
with `harnesses/corpus_bench.py --stanli-only --filter MODEL --timeout 900`,
using `build-candidate/bench_grad` and `build-candidate/stanli_run` for the
harness's BENCH and RUN paths. Each row is one warmed arithmetic-mean gradient
loop, one preparation measurement, and one 1,000-warmup/1,000-draw run (seed 1),
following the published corpus workflow. This refresh is separate from the
repeated matched A/B experiment above. The existing CmdStan cells are retained.

| Model | Gradient | Preparation | Source-to-CSV | Gradient evaluations in sampling |
| --- | ---: | ---: | ---: | ---: |
| gp_regr | 2669 ns | 0.000389 s | 0.51 s | 11580 |
| gp_pois_regr | 2274 ns | 0.000417 s | 0.77 s | 288594 |
| hierarchical_gp | 19566 ns | 0.013501 s | 13.86 s | 491205 |

The optimizer-comparison TSV reuses these same stanli gradient/preparation
observations, since its stanli compiler pipeline is identical; its CmdStan
measurements remain separate. `accel_gp` contains unused GP/Cholesky function
definitions but executes its spectral approximation, so its row is unchanged.
The gp_regr source-to-CSV observation increased from 0.08 to 0.51 seconds; it
is retained rather than inferred from the improved fixed-point gradient time.

Regenerated the full and representative tables and generated README/web
fragments. Recomputed the corpus summaries from the TSV, correcting stale
handwritten medians: 2.10x gradient throughput and about 8.6x source-to-CSV.
Validation: `python3 tools/gen_docs.py --check` and `git diff --check`.
