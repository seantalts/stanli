# Linux allocator default rollout

## Current decision and authorization

The user requested one PR that enables a useful default, with the necessary
testing. PR #363 consolidates the integration and corrected evaluator; #364
is superseded. The rebased baseline is main `10f94763`. The original dirty
research worktree and [complete earlier evidence](https://github.com/seantalts/stanli/tree/4e2d1bb4da8b7b602c73baaab875bf78f5c7b439)
are preserved. Auto-merge is paused and the PR is a draft until this gate is
resolved.

Candidate: private mimalloc 3.5.1 for supported Linux x86_64/ARM64 native
shared libraries. Fresh Linux builds select AUTO; explicit/cached SYSTEM is
preserved. Unsupported architectures, sanitizers and LTO fall back to SYSTEM.
Windows/macOS defaults stay SYSTEM. Static/CLI, web/WASM and loop layout are
unchanged. No Stan Math/Eigen source changes, C++ new/delete override,
process interposition, alternate TLS or allocation-size fallback.

## Frozen validation plan (before timings)

1. Build SYSTEM and the platform-default candidate from the same source,
   flags, dependency pins and embedded compiler. Record source/build/binary
   identities and host/cgroup/compiler information. Run the full native and
   Python suites for both. Keep normal wheel, corpus, R, ownership/export,
   browser and sanitizer CI. Local macOS remains a fallback compatibility
   check, not evidence of Linux speed.
2. On both shipping manylinux architectures, verify full gradient bytes and
   graph dimensions across the 23 pinned fixtures, 1/4 workers, plus six
   predefined 8-worker cases (52 cells). Warm each worker for at least 500 ms
   of actual native gradient work. Retain preparation and first-gradient
   costs separately; do not add shipping warmup.
3. Run five counterbalanced rounds, two processes per slot, three blocks
   per process and both identical-binary A/A controls: 2,080 timed processes
   per architecture. Baseline-only 150 ms calibration is frozen. Report all
   ratios, ranges, signs and peak memory; no aggregate hides a loser.
4. Use the retained diagnostic trigger: every median ratio below 0.97 plus
   Eight Schools/1, normal 1024/4 and hierarchical GP/4 gets one fixed
   seven-round confirmation, unchanged binaries and calibration. A repeatable
   unexplained >=3% loss blocks promotion; 3% is not a permitted regression
   budget. Smaller consistent negatives and unstable A/A controls require
   explicit review. Do not retry numerical/performance failures for a better
   sign. Infrastructure retries must retain the failed attempt and exact
   reason.
5. End-to-end source-to-fit and sampling: Eight Schools, radon pooled,
   hierarchical GP, normal 1024/262144, gamma 128 and Lotka-Volterra;
   one/four chains, fixed seed 49201, 500 adaptation + 500 sampling
   transitions, default depth/delta and fixed zero unconstrained inits.
   Five counterbalanced rounds, one process per slot/round with both A/A
   controls (280 timed processes per architecture). Retain every draw,
   sampler statistic and diagnostic count; require bitwise identity across
   variants. There is no artificial benchmark warmup before sampling.
   Report process/import, preparation, sample and source-to-fit costs
   separately. The same fixed confirmation rule applies to >=3% sampling
   or source-to-fit deficits, with Eight Schools/1 and normal 1024/4 controls.
6. Run the fixed 12 small/large/small model-lifetime sequences in one NumPy
   host for each variant (1,179,648 timed gradients plus checks/warmup).
   Require complete snapshot parity; inspect RSS after blocks, join and
   destruction for a plateau. Unexplained continued growth blocks rollout.
   No forced collection or purge tuning.

This tests allocator ownership/placement as the candidate mechanism while
retaining startup sensitivity and non-allocator execution as alternatives.
The earlier Linux work supports sustained-gradient viability, not full
inference or all models. The earlier Apple loss is not solved or generalized
away. Eliminating temporaries via specialized kernels remains a deferred
architectural alternative; it is unnecessary to implement that different
design for this bounded allocator rollout.

## Delivery gate

Keep #363 as the single PR, with this plan, reproduction commands and compact
tables linked to retained raw artifacts. Enable auto-merge only after the
Linux performance/memory/numerical decision and applicable CI pass. If a
platform fails, report the exact failing cell and keep its default unpromoted;
do not silently redefine success as merely landing an opt-in integration.
