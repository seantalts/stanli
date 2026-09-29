# Compile uninitialized scalar integer declarations

The output-coverage refresh found one interpreted case among the 329 recorded
corpus models: `declare-define-multi`, which reads and emits uninitialized scalar
integers. The compiled declaration path cleared the previous binding but left
no new value. MIR already returned Stan's `INT_MIN` sentinel.

The shared declaration path now installs that sentinel for an uninitialized
scalar integer. Later assignments still replace it. No new opcode, evaluator,
cache or runtime allocation was added. This completes compiled output coverage
for the current recorded corpus, not every Stan program or every MIR role.
The [per-model output report](data/2026-09-29-output-coverage.txt) records all
329 selections using the existing `verify_refs.py --wa-report` command.

## Correctness

- Existing reused-symbol regression now requires compiled execution and exact
  full-row agreement with MIR after shadowing an earlier runtime integer sum.
- New `gq_uninitialized_int` fixture checks direct output, copied and promoted
  reads, conditional assignment on both branches, later overwrite, uninitialized
  loop locals and a following RNG draw. Its independent pinned CmdStan 2.40.0
  reference has **36 values, maximum 1 ULP**; no tolerance changed.
- Full **314/314 CTests** and **329/329 recorded corpus models** pass, comparing
  **1,020,194 values**, including 124 existing platform ULP gates. The existing
  cancellation-sensitive corpus maximum of 7,040 ULP is unchanged; the full
  corpus uses its documented mixed contracts, not a universal 10-ULP guarantee.
- The declaration conformance model's 30 non-finite outputs are retained.
  Tests still enforce conservative array-sum initialization/range guards.
- Installed Python and R interface suites pass against the final library;
  one platform-inapplicable R test reports an empty test and is skipped.
  Formatting and whitespace checks pass. Upstream was fetched again before
  submission and remained at `d13f7fa9`; ancestry was verified.

## Native measurements

Baseline: integrated commit `d6449fbf` (including upstream `d13f7fa9`); candidate:
that source plus this scalar-initialization change. Native Release, Darwin arm64,
Apple Clang 21, full runtime with threads. Six alternating fresh-process pairs,
200 ms warmup and 250 ms measurement windows, no concurrent builds/tests.

| Workload / phase | Baseline median ± MAD | Candidate median ± MAD |
| --- | ---: | ---: |
| Small proper-prior fixture: prepare | 131.8 ± 4.3 µs | 144.0 ± 1.7 µs |
| Small fixture: warm output row | 5.71 ± 0.12 µs | 0.497 ± 0.003 µs |
| Small fixture: 100 warmup + 100 draws and output | 914.8 ± 63.8 µs | 334.0 ± 2.7 µs |
| Declaration conformance: prepare | 5,721 ± 71 µs | 5,948 ± 81 µs |
| Declaration conformance: warm output row | 325.3 ± 10.8 µs | 1.46 ± 0.04 µs |
| Ordinary AR1: inference and output | 8,390 ± 47 µs | 8,361 ± 149 µs |
| Compiled binomial container: inference and output | 3,044 ± 34 µs | 3,037 ± 38 µs |

The small fixture recovers added preparation in about three output rows;
prepare plus short inference improves from 1,047 to 478 µs. The conformance
fixture recovers added preparation in one warm row. It has improper flat priors,
so inference was deliberately not timed.

The small fixture's first gradient increased from 5.3 to 11.9 µs; first output
decreased from 29.5 to 21.6 µs. Its warm gradient changed 0.585→0.613 µs;
the empty-density conformance model changed 0.643→0.677 µs. These phase-level
costs are recorded, not hidden by the output gains. The ordinary AR1 and already
compiled binomial canaries show no clear complete-run regression. This follows
the user's normal-use policy; it is not proof that every phase or possible model
is unchanged. Unchanged source compilation also varies between processes.

Peak process RSS was 27.31→27.51 MB for the small fixture and 34.62→34.35 MB
for the conformance model. Both binaries are **39,831,040 bytes**. No separately
retained-memory measurement was made. The change adds one existing-map entry
per previously unbound declaration during preparation.

[Raw samples, summaries, hashes and protocol](data/2026-09-29-uninitialized-int-performance.json).
The new fixture is in the existing execution benchmark manifest; public corpus
benchmark tables were not changed.
