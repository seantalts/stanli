# Sampler startup alignment

Scope: the user requested inexpensive alignment with CmdStan after the ctsem
end-to-end comparison exposed different trajectories. Base is fetched upstream
`c260534bed071775fc703ce765a096273b21dee7`; branch is
`codex/nuts-startup-parity`. This changes sampler setup, not model numerics.

Stanli now starts step-size search at CmdStan's requested default of `1.0`
and sets the initial dual-averaging center to `log(10 * 1.0)` before search.
Previously it inherited `base_hmc`'s `0.1` default and centered adaptation on
the searched step size. Adaptation is engaged before search, matching Stan's
service ordering. Existing seeded draws will consequently change.

## Differential proof

`tests/test_sampler_parity.cpp` runs normal and eight-schools executors through
both Stanli's driver and Stan's actual `hmc_nuts_diag_e_adapt` service. Each
driver gets an independent executor with identical numerical implementation.
The four configurations cover zero, short and metric-adapting warmup, saved
warmup, thinning, chain IDs, acceptance targets and zero initialization.
All eight cases failed before the fix. Afterward all 430 saved rows (every
parameter and all seven sampler diagnostics) match bitwise, with no tolerance.
There are no random generated quantities in this oracle.

Validation, Release with `STANLI_THREADS=ON`:

```sh
cmake --build build-rel --target test_sampler_parity test_nuts test_sampling test_multichain test_e2e test_capi stanli_run -j 4
ctest --test-dir build-rel --output-on-failure -R '^test_(sampler_parity|nuts|sampling|multichain|e2e|capi|run_timings)$' -j 4
```

All seven focused suites pass. The bounded scope uses the sampler, multichain,
API and CLI suites rather than rerunning unrelated compiler tests or full
inference benchmarks.

## Actual ctsem startup

The retained N=4000 model/data and CmdStan binary were compared with the rebuilt
Stanli CLI: seed 1, chain 1, one saved warmup transition, zero post-warmup
transitions, depth 1. Both use step size `0.0078125`, acceptance `1`, one
leapfrog and no divergence. All 7,946 diagnostics, parameters and transformed
parameters pass the unchanged scaled-error gate of `1e-9`; the maximum is
`1.108464985173096e-11`, in `pop_asymDIFFUSIONcov.1.1`. Only 1,493 of those
values match bitwise. The first transition precedes generated-quantities RNG
consumption, so it isolates startup rather than testing later stream alignment.

Commands, binary hashes and diagnostics are in the
[scorecard](2026-09-20-sampler-startup-parity.json); raw CSVs, stderr and the
driver are retained in `.cache/nuts-startup-parity-20260920/`.

## Remaining gap

CmdStan evaluates random generated quantities between saved transitions on
the sampler's RNG. Stanli's CLI evaluates output afterward with a fresh stream;
the C API's in-worker writer also owns a separate stream. Aligning that requires
changing callback plumbing and CLI output scheduling, so it is deferred from
this small fix. The first discriminating follow-up is a random-GQ fixture
compared draw by draw against Stan's service. Cross-compiler numerical
differences remain even after stream alignment. No new end-to-end speedup or
complete ctsem trajectory parity is claimed.
