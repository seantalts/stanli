# Sampler and generated-quantities RNG alignment

User objective: match CmdStan's RNG consumption without a performance
regression. Work follows startup alignment on `codex/nuts-startup-parity`,
baseline `dfc6d05b9c8f9659d0be6c7825c438f5ad7d7f4e`, with fetched default branch
`c260534bed071775fc703ce765a096273b21dee7` verified as an ancestor.

## Plan and single review

One read-only plan review ran through `claude --model claude-fable-5-1`.
The confirmed model, complete response and prompt are retained under
`.cache/sampler-rng-parity/`. No second review or delegated implementation
was requested. The review changed the plan as follows:

- Let the chain own `WaRng`, supplying its engine to Stan's sampler and its
  wrapper by reference to the saved-draw callback. There are no copies,
  reseeds or rollback of RNG state at the callback boundary.
- Use a test-only RNG-aware reference model for the actual Stan service;
  keep optimizer and Pathfinder output callbacks unchanged. The reference
  implements generated quantities independently using Stan Math, rather
  than calling Stanli's output executor.
- Catch generated-quantity domain errors in full-output C API sampling,
  emit NaNs, and continue. This deliberately changes the previous C API
  contract, which aborted the chain. Other exceptions still fail the chain.
- Format CLI CSV once per draw. Chain zero writes directly when its schema
  is known; subsequent chains use buffered, automatically deleted temporary
  files and are copied in chain order. Memory does not grow with output width
  times draw count unless the user requests a summary.
- Probe interpreted output columns with the existing shared points and a
  scratch RNG. Successful discovery makes the interpreter read-only across
  chains. If probes fail, each chain discovers independently and earlier
  error rows are padded when the final schema is known. Failed discovery
  attempts now clear their partial column lists before retrying.
- Use one output path, without speculative compiler analysis to bypass it
  for RNG-free models. Measure actual overhead before retaining the change.

## Correctness contract and evidence

Generated quantities run only for stored transitions, including saved warmup.
Dropped transitions do not consume their RNG draws. Parameter-only C/C++
sampling still skips generated quantities, so its seeded trajectories differ
from full-output sampling on random-GQ models. Standalone generated-quantity
and BridgeStan RNG handles retain their independent streams. C ABI signatures
are unchanged; C++ stored-draw callbacks gain live RNG and diagnostic arguments
and require recompilation.

The actual Stan `hmc_nuts_diag_e_adapt` service is the differential oracle.
Identical independent density executors isolate RNG scheduling from compiler
rounding differences. The original 430 no-GQ rows still match bitwise. Twenty-four
new cases add 1,536 rows across scalar RNG families, vectorized normal RNG,
compiled and interpreted output, zero/short/adapting warmup, thinning, chain
IDs and zero initialization. All seven diagnostics, unconstrained parameters
and successful GQ rows match bitwise. There are 242 rejected output rows,
including rejections after RNG consumption; subsequent transitions still
match. CmdStan preserves already-written prefixes of failed rows, whereas
Stanli fills the whole output row with NaNs. This is deliberately excluded
from the claim of identical failed-row CSV contents.

`test_run_rng` compares the CLI with the shipped C ABI, covering explicit
initial vectors, saved warmup, thinning, serial/parallel chains, both output
backends, and summary/statistics flag independence. It also compares a model
whose header probes all fail with a reference that consumes the same RNG,
checking late schema discovery and NaN prefixes with and without diagnostics.
`test_csv_writer` checks ordered spool copying across buffer boundaries and
empty streams. C API tests retain the RNG-free and null-output canaries and
verify that random GQ now changes the subsequent sampler trajectory.

A clean Release rebuild with `STANLI_THREADS=ON` passed. The full configured
suite has 264 passes and two pre-existing failures:
`test_density_signature_model_generation` and
`test_builtin_signature_model_generation`, both stale manifest checks. The
same messages are retained in `.cache/ctsem-post-392/final-ctest.log` from the
baseline work. Current logs are in `.cache/sampler-rng-parity/`.

## Performance results

Frozen baseline and candidate binaries are compared in whole CLI processes,
including source preparation, sampling, generated quantities, formatting and
file output. Ten counterbalanced pairs follow one discarded warmup pair;
case order rotates. CSV hashes and gradient counts must match on RNG-free
controls. Random-GQ Gaussian controls use depth one and no warmup, fixing
the leapfrog and GQ call counts even though the trajectories change. Wall time,
RSS, binary/source hashes, commands and all repetitions are retained.

A repeatable slowdown above 2% triggers investigation; 2% is not an accepted
regression budget. Ambiguous results need an identical-binary A/A control.
Any mitigation gets a bounded, separately frozen confirmation experiment;
negative results are retained. `sample_s` now includes output work, so phase
times cannot be compared as though their boundaries were unchanged.

The first candidate passed byte/gradient controls but regressed the wide
single-chain control by 3.0%, scalar random output by 1.9%, vector random
output by 3.8%, and interpreted output by 2.4% (paired median wall ratios).
The plain control was unchanged and parallel wide output was 58.4% faster.
These results are retained, not discarded, in
`.cache/sampler-rng-parity/performance/`.

One mitigation removes extra output-row materialization for compiled CSV and
the CLI's unused unconstrained draw matrix. The latter uses an opt-in C++
`retain_draws=false`; other callers keep the existing default. Callbacks and
statistics still cover every stored draw, verified against the Stan oracle.
The confirmation repeats ten counterbalanced pairs, increases the short
interpreted control from 100,000 to 300,000 draws, and then performs six
preselected identical-binary A/A pairs on plain, scalar-random and interpreted
controls. The commands and decision policy are frozen before timing in
`.cache/sampler-rng-parity/confirmation/`.

The final CLI paired medians are below. Ratios below one favor the candidate.
All RNG-free CSV hashes agree; gradient counts agree on every control.

| Control | Baseline median (s) | Candidate median (s) | Median paired ratio |
| --- | ---: | ---: | ---: |
| Plain, 300,000 draws | 0.8937 | 0.8806 | 0.9844 |
| Wide, 120,000 draws | 1.0962 | 1.0562 | 0.9646 |
| Wide, four parallel chains of 40,000 | 1.2244 | 0.4865 | 0.4019 |
| Scalar random GQ, 600,000 draws | 0.9027 | 0.8620 | 0.9543 |
| Vector random GQ, 250,000 draws | 0.9732 | 0.9726 | 1.0004 |
| Interpreted random GQ, 300,000 draws | 1.1069 | 1.1216 | 1.0135 |

Median peak RSS decreases by 38–54% across these six controls. Per-case
dispersion and all raw repetitions are retained in the scorecard.

Identical-binary A/A paired medians are 1.0003 (plain), 1.0041 (scalar
random), and 1.0053 (interpreted), with individual ratios ranging from
0.9773 to 1.0334. The remaining interpreted CLI signal is small and
inconclusive; this does not prove its cost is exactly zero. No material
regression remains on the compiled path used by ctsem. The candidate's
random trajectories also produce different CSV text lengths, despite equal
leapfrog and RNG-call counts, so those controls do not hold formatting bytes
identical. The vector case writes about 2.1% more bytes in the candidate.

Separate C API measurements use eight counterbalanced pairs, 400,000
depth-one transitions per process, and time only the full-output sampling
call with caller buffers allocated beforehand. The paired ratios are 0.9678
(plain), 1.0002 (vector random GQ), and 1.0087 (interpreted random GQ).
The interpreted ratio of median times is 1.0005 (1.2293 versus 1.2287 s),
illustrating the small measurement dispersion. All runs perform exactly
400,000 leapfrogs, and the RNG-free output hashes agree. These results support
effectively neutral RNG wiring, not a universal zero-overhead guarantee.

The first regressing candidate and the full confirmation/A/A/API records
remain available under `.cache/sampler-rng-parity/`. Python's 50 tests and
the R sampling suite's 98 assertions pass with the new shared library.

## N=4000 trajectory

One retained seed-1 run uses 30 warmup and 30 saved transitions at depth five,
with the same model, data, initialization radius, thread count and output
precision as the previous comparison. Selected saved densities (first,
middle, last) were replayed in the retained CmdStan executable with the
unchanged scaled-error gate of `1e-9`. All three pass, with maximum scaled
error `7.37e-16`; all 30 CSV rows and all 8,736 columns are finite.

The first saved parameter/transformed-parameter row is bitwise unchanged
from pre-change Stanli. The second is now within `4.74e-6` scaled error of
CmdStan, compared with approximately `0.79` before sharing the RNG. All 30
draws match CmdStan's tree depth, leapfrog count and divergence flag. Both
have 923 post-warmup leapfrogs, two divergences and 28 draws at the depth
limit. Across parameters and transformed parameters, the largest scaled
difference over the full run is `1.97e-4`, at draw 22; the final draw's
maximum is `3.55e-5`. This is close trajectory agreement, not bitwise equality.

| Run | Whole-process wall | Stanli gradient evaluations | Post-warmup leapfrogs |
| --- | ---: | ---: | ---: |
| Prior Stanli, separate GQ stream | 581.983 s | 1,330 | 842 |
| Stanli, live GQ stream | 614.616 s | 1,411 | 923 |
| Retained CmdStan | 1,334.159 s | — | 923 |

The new observation is 10m15s versus CmdStan's 22m14s, or 2.17x faster. It
takes 5.6% longer than prior Stanli while requesting 6.1% more gradients.
That reflects the aligned trajectory doing different work; the paired
controls above assess implementation overhead. CmdStan and prior Stanli
timings are retained historical runs, not a fresh paired speedup estimate.
This short, depth-limited run with divergences is a performance/trajectory
diagnostic, not a claim of converged inference.

## Reproduction and delivery

The [scorecard](2026-09-20-sampler-rng-parity.json) contains source and binary
hashes, fixture source text, commands, every timed repetition, dispersion,
the oracle transcript and the ctsem density checks. Local raw artifacts are
under `.cache/sampler-rng-parity/` and
`.cache/ctsem-rng-seed1-20260920/`. The exact frozen drivers are `bench.py`,
`confirm.py`, `api_bench.py`, and `ctsem.py` in the first directory. They
refuse to overwrite completed runs.

Validation commands:

```sh
cmake --build build-rel --clean-first --target stanli_run stanli_shared -j 6
cmake --build build-rel -j 6
ctest --test-dir build-rel --output-on-failure -j 6
ctest --test-dir build-rel --output-on-failure -j 4 -R '^(test_sampler_parity|test_capi|test_csv_writer|test_write_array|test_multichain|test_sampling|test_model_adapter|test_nuts|test_run_rng|test_run_timings)$'
```

The final ten focused suites pass, including the expanded 1,966-row oracle.
The full suite and binding results above apply to the same production code.
