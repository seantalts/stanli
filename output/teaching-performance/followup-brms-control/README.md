# brms control-flow support and performance follow-up

These are separate measurements on Apple M3 Ultra, macOS ARM64, using a Release
build. They do not replace rows in the published 199-model sweep. Runtime changes
are in `63d32575`, `3516e961`, `ff17292a`, and `8923ccee`; each experiment retains
its exact measured patch and executable hashes.

## Numerical differences

COM-Poisson now compiles and evaluates, including integer `%`, runtime vector
writes, and a log-sum-exp over a runtime prefix. The table shows maximum absolute
differences from independent CmdStan references across three parameter points.

| Model | Log density | Gradient | Model outputs |
| --- | ---: | ---: | ---: |
| COM-Poisson | 2.84e-14 | 2.13e-14 | 0 |
| Asymmetric Laplace | 1.42e-14 | 1.78e-14 | 0 |
| Zero-inflated asymmetric Laplace | 4.26e-14 | 1.42e-14 | 0 |

[Paired values and ULP differences](final-numerics.json) are retained. COM-Poisson
outputs use a freshly built CmdStan reference driver because the original
reference file contained only its density and gradient. No reference values or
numerical tolerances were changed.

## Complete-process sampling

Ratios are **CmdStan/Stanli**; above one means Stanli is faster. Each entry uses
four seeds, 1,000 warmup and 1,000 retained draws, with the unchanged
min(3 × matching CmdStan duration, 900 seconds) cap. Stanli source compilation
is included; CmdStan model compilation is excluded. This is fixed-budget
runtime, not time to equal inferential accuracy.

The confirmation experiment launches each executable once without arguments
before measuring complete sampling processes. Thus these are **warm-executable
CLI measurements**, not first-install times. The two Stanli versions alternate
order between seeds; CmdStan runs first to establish each seed's cap.

| Model | Before input-copy optimization | After input-copy optimization | Completed seeds, each engine |
| --- | ---: | ---: | ---: |
| Asymmetric Laplace | 0.661× | 0.775× | 4/4 |
| Zero-inflated asymmetric Laplace | 0.645× | 0.772× | 4/4 |

Both remain slightly below the 0.8 target in this confirmation. All eight
before/after pairs produce [byte-identical draw CSVs](window-draw-identity.json).
Both models have zero divergences in both engines; maximum R-hat and minimum
bulk ESS are retained in [sampling diagnostics](window-diagnostics.json).

The initial experiment is also retained: the first asymmetric-Laplace run
timed out in both newly copied Stanli executables before producing output.
Zero-inflated Laplace completed all four seeds, measuring 0.719× before and
0.843× after. These observations are not spliced into the confirmation medians.
A separate [no-argument launch probe](executable-startup.json) measured a newly
copied Stanli executable at 0.468 seconds initially and 0.00575 seconds on its
next launch; CmdStan measured 0.156 and 0.00888 seconds. This demonstrates a
startup cost that matters for such short runs.

COM-Poisson still reaches the relative sampling cap on all four seeds after
the two replay optimizations. There is no completed-run Stanli median or CLI
speedup to report for it.

## Gradient measurements

Five counterbalanced trials per model and engine use 200 ms warmup and 400 ms
measurement. Sharing uniform constants improves COM-Poisson gradient evaluation
by 2.57× versus the first working implementation; removing inactive derivative
work adds 1.11× in a separate paired experiment. It remains about five times
slower than CmdStan for this gradient workload.

Copying only the cells read by conditional calculations reduces asymmetric
Laplace gradient time by 19% and zero-inflated Laplace by 18%. Their measured
CmdStan/Stanli gradient ratios are 0.707× and 0.860×. These are distinct from the
complete-process ratios above. Values and gradients are identical to the
previous runtime; the GEV and Rethinking m14.8 canaries are included in the raw
trials. No improvement is established for GEV.

## Evidence and reproduction

- [Runtime checks](runtime-tests.txt): 252 tests, including signed `%`, zero
  divisor, dynamic bounds, constant bit patterns, and input-window regressions.
- [Corpus replay](numerical-replay.txt): 316 models under the existing numerical
  and domain policies, 1,008,755 values at three points. Those policies remain.
- [R acceptance](r-ecosystem-tests.txt): 456 expectations, no failures, warnings,
  or skips.
- [Gradient experiments](gradient-experiments.json.gz): every trial, range of
  measured times, values, executable hashes, and source patches.
- [Sampling experiments](sampling-experiments.json.gz): every completed/capped
  run, duration, cap, command, input hashes, source patches, and run identity.
  Includes all three COM-Poisson stages and both Laplace experiments.
- [Provenance](provenance.json): final build identity, independent reference
  build commands, and retained raw-data locations.

The commands in the sampling records reproduce each invocation. The full
reference replay uses `python3 tools/verify_refs.py deps/posteriordb --check
BUILD/stanli_check --jobs 8 --per-model`; runtime tests use `ctest --test-dir
BUILD --output-on-failure`. See [TESTING.md](../../../TESTING.md) for build setup.
Raw draw CSVs and original command logs remain under the separate local
directories recorded in the sampling evidence; the original full-sweep archive
is unchanged.
