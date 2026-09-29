# Harness execution gates

Review: `harnesses/vectorize_ab.py:794`'s op-count gate rewards collapsing
graph ops even though one island can hide arbitrarily more work than several
graph ops did; performance should be gated by execution and storage
measurements, with op counts kept as diagnostics. Also: a tuner that spends
1,000 gradient-times for a 5% win needs about 20,000 subsequent gradients to
repay it, so tuning cost has to be weighed against measured gain.

## 1. What the gates catch today, and what each misses

`op_count_gate` (vectorize_ab.py:792) compares the off/reroll-on and
on/reroll-on cells for any model whose portable MIR differs: lowered
log_prob ops must not grow, final log_prob ops may grow at most 10%. It is
exact (no run-to-run noise) and the only thing that fails a run today;
`manifest["gradient"]["gating"]` is `False`, matching the docstring and
TESTING.md, which both call gradient and prep timings evidence, not gates.

What it catches: a pass that turns a small graph fragment into a much larger
one. What it misses, structurally:

- A join that inflates cost per op. `aa00ce86` let one vector add end an
  island run without joining it to scalar form; islands got bigger and op
  counts shrank, but ten brms models measured slower. `4aeb267e` fixed it
  by pricing three carvings and picking the cheapest, recording the
  intermediate state as "4 to 13 percent slower on eleven brms models whose
  op counts had improved": fewer ops, slower gradient, invisible to the gate.
- A runtime change that slows both cells identically. The on/off comparison
  runs one `bench_grad` binary against both MIR files, so a regression in
  island carving, the interpreter, or `bind_` cancels out of their ratio.
  Only a fixed baseline (a prior binary, not the paired off cell) sees it.

The two runs on disk show the milder, first case directly: `ab_final4`
(before `3704a217`) fails on `s2_logistic_normal` because the pass makes a
zero-trip loop evaluate its right-hand side, growing lowered ops with no
runtime effect; `ab_final5` (after `3704a217` added
`LOWERED_GROWTH_EXPECTED`) passes the same model. Op counts needed a named
exception the first time a legitimate grower appeared, the same way
`4aeb267e` needed a cost-based fix the first time a legitimate slowdown
appeared.

## 2. The noise floor

`ab_final4` and `ab_final5` run the same 254-model corpus at the harness's
default gradient settings (`rounds=2`, `calibration_n=8`,
`target_seconds=0.75`, `prep_samples=2`). The only code difference between
them is three commits, including a reroll prefilter, that changed no op
counts, so their gradient and prep columns are close to a pure noise sample
for the 26-model `GRADIENT_MODELS` set, which is exactly the set of models
whose MIR changes under `vectorize-loops`. CI runs `--timeout 30` where
these two runs used `--timeout 120`; that bounds hangs only and does not
affect the numbers below.

| measurement | source | n | median | p90 | max |
| --- | --- | ---: | ---: | ---: | ---: |
| gradient on/off ratio, run5 vs run4 (the gate's statistic) | cross-run | 26 | 0.29% | 1.18% | 2.53% |
| gradient on and off, run4 vs run5 separately | cross-run | 26 | ~0.5% | ~1.2% | ~3.7% |
| gradient 4-value range within one run | within-run | 26 | 1.1-1.4% | 2.3-2.6% | 3.4-8.0% |
| `driver_total_ns`, run4 vs run5 | cross-run | 254 | 1.5% | 4.9% | 15.8% |
| `log_prob_total_ns`, run4 vs run5 | cross-run | 254 | 2.2% | 6.3% | 23.1% |
| prep, 2-sample range within one run (both fields) | within-run | 254 | 1.4-2.0% | 4.3-7.7% | 13.1-19.2% |

The within-run rows corroborate the cross-run ones as noise, not an
artifact of the two revisions differing. At current gradient settings, a
real 5% change is 2x the worst observed noise and 4x its p90 on the
26-model set, so 5% is already resolvable per-model. The catch is
family-wise: 26 models against a p90-sized
threshold trips about two per clean run, and the observed max (2.53%)
would trip a "2x p90" rule outright. Fix: threshold with margin above the
observed max (around 1.035-1.04, still far under the 4-13% case), plus one
confirmation re-run of a failing model (~6 seconds) before failing the job.
For the post-submit full run, doubling `--gradient-rounds` from 2 to 4 adds
about 2.6 minutes for the 26 gradient models, tightens the noise floor by
roughly root-2, and stays well inside the 25-minute step timeout.

Prep timing cannot support a tight gate at `--prep-samples 2`: p90 is
already 5-8% and the max exceeds the 10% op-count allowance. It needs many
more samples per cell (a per-model cost, not batched) or a much wider
margin than the gradient gate's.

## 3. Proposed gates

- **Op counts**: keep as computed, stop failing the run on their own.
  Report in `summary.md`/`summary.json`, and keep
  `LOWERED_GROWTH_EXPECTED`-style named exceptions for a MIR change whose
  op count grows for a semantic reason.
- **Gradient-time gate**, pass-on vs pass-off, on `GRADIENT_MODELS`: fail
  when `on_over_off` exceeds 1.04 (about 3.5x the measured p90 noise, well
  under the 4-13% case), with one confirmation re-run of the failing model
  before failing the job. This threshold comes from the arm64 macOS noise
  floor above and should be re-derived once the same measurement exists on
  the actual `linux-x86_64` CI runner, which is noisier.
- **Gradient-time gate against main**, for the class the on/off comparison
  cannot see: compare the pass-off cell's gradient time against the most
  recent post-submit run's pass-off cell for the same model. The "Publish
  MIR vectorization measurements" step (wheels.yml:560) already uploads
  `mir-vectorization-measurements` on every post-submit run; a later job
  fetches the latest one from `main` (`gh run list --branch main --workflow
  wheels.yml --status success -L1`, then `gh run download`) and diffs
  against it. Starts report-only, self-calibrated from N consecutive
  main-branch artifacts as in section 2, and gates only once that history
  exists. On the first run, or a missing artifact, skip the comparison and
  say so; never fail for a missing baseline.
- **Prep-time**: report only, per the noise floor above; a future gate
  needs more samples or a batched-invocation redesign.
- **Storage**: two Python-only measures. `slots` (the executor arena's
  element count) is already parsed into every prep row with zero
  run-to-run noise, so it can gate like an op count once exported into
  `bench.tsv`/summary (today it stops at `graphs.jsonl`). Peak resident
  memory needs `subprocess.Popen` plus `os.wait4(pid, 0)` in place of
  `subprocess.run`, which discards rusage; `wait4` returns the child's
  `ru_maxrss` (bytes on macOS, KB on Linux) for free, since every measured
  tool is already a direct child. Report both first; gate `slots`
  immediately and RSS only after a noise pass, since RSS carries allocator
  and page-cache noise that `slots` does not.

## 4. The tuner

`b8428f4d` made tuning opt-in (`STANLI_TUNE`, unset by default); the harness
runs tuning-off throughout, so every number above is the production path. A
tuned cell would need `STANLI_TUNE=1` threaded through `graph_cell` and
`gradient_benchmark` as a fifth combination. `b8428f4d`'s budget is up to
1,000x a warm gradient evaluation per island choice, up to 7 rounds; on the
26 models' own gradient times that bound is under 2 seconds total for one
choice each, but real models can carry many choices and this cannot be
bounded without running it. The reviewer's repayment point is about tuner
*adoption* in a deployed model, not CI cost, so a tuned cell (26-model set,
gradient-only) is worth measuring as a bounded follow-up before deciding
whether to gate it, staying report-only regardless since `b8428f4d`'s
agreement check only samples three points per choice.

## 5. Migration

Each commit keeps the PR-slice and post-submit jobs green.

1. Export `slots` into `bench.tsv`/`summary.json`; no behavior change, one
   added sentence in TESTING.md.
2. Add the pass-on/pass-off gradient gate at 1.04 with one confirmation
   re-run, `gating: True` in the manifest; update the docstring
   (vectorize_ab.py:16-17) and TESTING.md:108-111, both of which currently
   say gradient timings are never gates.
3. Add `os.wait4`-based `maxrss_bytes` to `run_command`'s return value
   (additive; existing mocks build the dict themselves) and report it in
   `bench.tsv`.
4. Add the main-baseline fetch as a post-submit-only step in wheels.yml,
   report-only, tolerant of a missing artifact.
5. Once N post-submit runs exist on `main`, derive and land its threshold,
   and remove the vestigial `op_count_failures` wiring from
   `write_reports`'s `ok` computation.
