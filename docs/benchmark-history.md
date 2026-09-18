# Historical benchmark experiments

These retained measurements describe their recorded revisions, inputs and protocols.
They are not a new sweep of the unified corpus. New measurements use the
[common benchmark runner and protocol](benchmark-protocol.md); source collection
names are provenance and optional filters.

Other retained runs include the [September 11 posteriordb tables](benchmarks.md#historical-posteriordb-measurements)
and the [September 16, 199-fixture appendix](../output/teaching-performance/README.md).
The latter retains its original collection-oriented report and all failures.
Do not pool these runs into a single median or substitute later results into them.

## Complete CLI sampling: 14 September 2026

This 13-model run used the Aalto source collection. The table
below measured the complete Stanli command, including source compilation,
JSON preparation, 1,000 NUTS warmup iterations, 1,000 saved draws, generated
quantities and 17-digit CSV output. CmdStan uses an already-built executable
with `--O1` and `-O3`; its build time is excluded from these speedups.

Measured on 2026-09-14 on the Apple M3 Ultra with the Release implementation
at `e46e360f`. Values are medians of five alternating pairs after one warmup
pair, with the same seeds and initialization at unconstrained zero. These
are the supplied teaching fixtures; seven use synthetic data.

<!-- educational-results:start -->
| model | stanli source-to-CSV | compiled CmdStan run | speedup | required floor |
| --- | ---: | ---: | ---: | ---: |
| `aalto_bern` | 12.08 ms | 16.25 ms | 1.345x | 0.5x |
| `aalto_binom` | 11.24 ms | 12.94 ms | 1.151x | 0.5x |
| `aalto_binom2` | 12.94 ms | 16.33 ms | 1.262x | 0.5x |
| `aalto_binomb` | 10.94 ms | 12.92 ms | 1.181x | 0.5x |
| `aalto_gpareto` | 30.75 ms | 32.97 ms | 1.072x | 1.0x |
| `aalto_grp_aov` | 16.54 ms | 21.74 ms | 1.315x | 0.5x |
| `aalto_grp_prior_mean` | 24.79 ms | 33.49 ms | 1.351x | 0.5x |
| `aalto_grp_prior_mean_var` | 64.09 ms | 91.37 ms | 1.425x | 0.5x |
| `aalto_lin` | 22.88 ms | 33.56 ms | 1.467x | 0.5x |
| `aalto_lin_std` | 18.23 ms | 30.41 ms | 1.668x | 0.5x |
| `aalto_lin_std_t` | 23.13 ms | 34.42 ms | 1.488x | 0.5x |
| `aalto_poisson_hurdle` | 653.72 ms | 871.43 ms | 1.333x | 0.5x |
| `aalto_poisson_simple` | 94.72 ms | 245.24 ms | 2.589x | 0.5x |
<!-- educational-results:end -->

Pareto's **1.072x** result has a modest margin: its median absolute deviation
is 0.662 ms versus 0.445 ms for CmdStan. The experiment required Pareto
at or above 1.0x and every other model at or above 0.5x. All 13 passed the
three-point log-density/gradient/generated-output oracle and sampling checks.
[Raw observations](../tests/educational/pareto-benchmark-results.json),
[fixture provenance](../tests/educational/IMPORT_README.md), and the
[detailed results](../tests/educational/RESULTS.md) retain methodology,
uncertainty and correctness evidence. These end-to-end measurements are
separate from the posteriordb measurements on the [main page](benchmarks.md).

## Fixed-point gradients: 14 September 2026

A separate run through the standard corpus drivers measures warmed gradients
at the same deterministic unconstrained point, with CmdStan `--O1`. These
are arithmetic means from one timed loop per engine/model, following the
[historical gradient method](benchmarks.md#benchmark-method); no sampling or build time enters the gradient ratio.

<!-- educational-gradients:start -->
| model | parameters | stanli gradient | CmdStan gradient | gradient speedup |
| --- | ---: | ---: | ---: | ---: |
| `aalto_bern` | 1 | 75 ns | 103 ns | 1.37x |
| `aalto_binom` | 1 | 70 ns | 114 ns | 1.63x |
| `aalto_binom2` | 2 | 123 ns | 186 ns | 1.51x |
| `aalto_binomb` | 1 | 66 ns | 90 ns | 1.36x |
| `aalto_gpareto` | 2 | 361 ns | 231 ns | 0.64x |
| `aalto_grp_aov` | 4 | 248 ns | 329 ns | 1.33x |
| `aalto_grp_prior_mean` | 6 | 301 ns | 413 ns | 1.37x |
| `aalto_grp_prior_mean_var` | 10 | 547 ns | 744 ns | 1.36x |
| `aalto_lin` | 3 | 175 ns | 282 ns | 1.61x |
| `aalto_lin_std` | 3 | 181 ns | 318 ns | 1.76x |
| `aalto_lin_std_t` | 4 | 270 ns | 367 ns | 1.36x |
| `aalto_poisson_hurdle` | 2 | 2.308 us | 58.015 us | 25.14x |
| `aalto_poisson_simple` | 1 | 840 ns | 956 ns | 1.14x |
<!-- educational-gradients:end -->

Pareto measures **0.64x** CmdStan gradient throughput at this point, while
its separately measured complete run reaches **1.072x**. Complete-run time
also includes preparation and output, and the samplers can take different
NUTS trajectories and gradient counts. The gradient result is not a claim
that every phase beats CmdStan. [Raw gradient observations](educational-bench-o1.tsv),
[compiler identities](educational-bench-o1.manifest.json), and
[driver/fixture hashes](educational-bench-o1.metadata.json) are retained.

The required-floor column records this experiment's policy. It is not a
special performance gate for the current corpus. The [detailed investigation](../tests/educational/RESULTS.md)
retains revision comparisons, numerical checks and raw observations.

To render the archived sampling table from its retained observations:

```sh
python3 tools/corpus_table.py --historical-sampling \
  tests/educational/pareto-benchmark-results.json
```

This renders historical data; it does not run a new experiment.
