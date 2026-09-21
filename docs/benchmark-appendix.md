# Benchmark experiment details

The [main table](benchmarks.md#full-corpus) uses default CmdStan compiler flags.
The appendix table below uses stanc3 `--O1` with loop vectorization enabled.
Both experiments cover the same application inputs with the same Stanli build.

## Configuration and evidence

| Item | Configuration |
| --- | --- |
| Scope | All 319 application models, `--corpus all` |
| Gradient trials | Six alternating pairs, 200 ms warmup and 250 ms measurement per process |
| Estimated workload | Setup + 20,000 × median warm gradient time |
| Stanli build | Release clang++, `-O3 -DNDEBUG`; full runtime, threads enabled, lite mode disabled |
| Upstream source base | `2c9d67b98b196e800dc051460aaaf653ac811ace`; exact measured checkpoint and hashes in manifests |
| Default reference compiler | Stock pinned stanc3, no additional flags |
| CmdStan | 2.40.0, `d3d5df6a22565edbe13edbd4eb40762cc8c5a4d6` |
| Stan Math | `5252d51d47c1d5e78005fc043ad996fad6dd8da8` |
| Timeouts | Gradient command 60 s; build 900 s |
| Host | Apple M3 Ultra, macOS arm64, 32 logical CPUs, 96 GiB RAM |

The sweeps run serially on a shared development host with normal background
services. Commands request one thread for Stan, OpenMP and common BLAS
implementations. The manifests retain machine details, requested threads,
source/data hashes, executable hashes, upstream identities and compiler flags;
event logs retain load averages and exact commands.

[Default evidence](../output/corpus-performance/README.md) and
[optimized-reference evidence](../output/corpus-performance-vectorized/README.md)
contain the complete inventories, immutable manifests, summary TSVs, raw paired
observations, command events and checksums. Every model remains in the table,
including numerical failures and timeouts. Current results are never filled
from older experiments.

## What the numbers include

**Warm gradients:** both engines evaluate the sampling log density with
proportional terms and Jacobian adjustment at coordinate `i` equal to
`0.1 + 0.05 * (i % 7) - 0.15 * (i % 3)`. Construction and the initial validation
evaluation are outside the timed window. Each accepted pair must agree on the
log density and every gradient component within scaled error
`abs(a-b) / max(abs(a), abs(b), 1) <= 1e-9`. Non-finite values, unequal widths
and failed commands are rejected. This gate is distinct from the independent
[three-point numerical replay](../TESTING.md#comparison-with-cmdstan-on-complete-models).

The gradient ratio is the median of six within-pair CmdStan/Stanli ratios,
with median absolute deviation (MAD). The two engine latencies are summarized
separately. Therefore the paired ratio can differ from a ratio of the medians.
Small differences should be read with their dispersion.

**Setup:** Stanli includes a source-to-MIR compiler-probe process and the
median of six preparation measurements from existing MIR/JSON to a bound
executor. CmdStan includes stanc translation and a fresh ordinary C++ model
build. CmdStan data/model initialization is not added to the estimate.
Common dependencies and precompiled headers are prepared in advance.
Gradient-driver compilation is retained in the event log but excluded from
the estimate. Compilation phases are measured once, with normal filesystem
caches; this is not an in-process Python/R or cold-cache first-fit measurement.

**Estimated time:** each engine's setup time plus 20,000 times its median warm
gradient time. The assumed workload is 2,000 iterations at ten gradients each;
no sampler is run, and adaptation, tree building, generated quantities and CSV
output are excluded. A missing setup component leaves that estimate blank
without hiding a valid gradient measurement. Sampler correctness tests remain
part of [the validation suite](../TESTING.md).

The fixed-point gate leaves four default-run rows without estimates:
`dogs_log`, `s2_invgaussian` and `sir` report non-finite density or gradients;
`kronecker_gp` has scaled error `0.0063`. The optimized-reference run also
rejects `s2_gp_by_gr` at `5.33e-8`. These rows retain their recorded failures;
see the [numerical policies and exceptions](../TESTING.md#comparison-with-cmdstan-on-complete-models).

## Reproduction

Prepare the compiler/dependencies and build `bench_grad` as described in the
[protocol](benchmark-protocol.md). Use distinct output paths for the two runs:

```sh
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  build/corpus-current/default-v4.tsv --bench build/bench_grad \
  --corpus all --rounds 6 --warmup-ms 200 --measure-ms 250 \
  --gradient-timeout 60 --build-timeout 900
python3 harnesses/corpus_bench.py deps/cmdstan deps/posteriordb \
  build/corpus-current/vectorized-v4.tsv --bench build/bench_grad \
  --corpus all --rounds 6 --warmup-ms 200 --measure-ms 250 \
  --gradient-timeout 60 --build-timeout 900 \
  --stanc build/corpus-current/stanc-o1vec-src/_build/default/src/stanc/stanc.exe \
  --stancflags=--O1
```

Run the commands serially. `--resume` requires identical sources, inputs,
binaries and settings. Publish the complete runs with
`tools/publish_corpus_bench.py`, then regenerate the docs and browser catalog:

```sh
python3 tools/publish_corpus_bench.py build/corpus-current/default-v4.tsv \
  output/corpus-performance
python3 tools/publish_corpus_bench.py build/corpus-current/vectorized-v4.tsv \
  output/corpus-performance-vectorized --compiler-evidence build/corpus-current
python3 tools/gen_docs.py
python3 tools/gen_web_models.py deps/posteriordb
```

## CmdStan with stanc3 loop vectorization

Stock pinned stanc3 leaves loop vectorization off at `--O1`. This experiment
changes that one setting and invokes the resulting compiler with `--O1`.
Compared with the default table, it enables **O1 optimizations plus loop
vectorization**; it is not a vectorization-only ablation or `--Oexperimental`.
Both engines are remeasured with the same inputs, Stanli binaries, timing
windows and numerical gate. The table focuses on warm gradient performance.

The [compiler provenance](../output/corpus-performance-vectorized/compiler-provenance.json)
records pinned source, build commands, tool versions and binary hash. The
[enabling patch](../output/corpus-performance-vectorized/stanc-o1vec.patch)
and stock/patched optimized MIR for a scalar normal-likelihood loop are retained
with the [evidence](../output/corpus-performance-vectorized/README.md).

<details>
<summary>Show all models against loop-vectorized CmdStan</summary>

<!--gen:benchmark_vectorized_catalog-->
Run `7bc507e14400797e`: 319 models, 6 alternating pairs. Gradient latencies are microseconds (median ± MAD); speedup is the median within-pair CmdStan/Stanli ratio ± MAD. Missing measurements are —.

| Model | Stanli µs | CmdStan O1+vec µs | Paired speedup ± MAD | Notes |
| --- | ---: | ---: | ---: | --- |
| `2pl_latent_reg_irt` | 67.37 ± 0.59 | 106.4 ± 2.3 | 1.59x ± 0.03 | complete |
| `GLMM1_model` | 10.24 ± 0.243 | 33.41 ± 1 | 3.23x ± 0.07 | complete |
| `GLMM_Poisson_model` | 0.6718 ± 0.0057 | 1.296 ± 0.038 | 1.88x ± 0.04 | complete |
| `GLM_Binomial_model` | 0.8716 ± 0.0243 | 1.165 ± 0.0206 | 1.33x ± 0.03 | complete |
| `GLM_Poisson_model` | 0.3776 ± 0.0128 | 0.7803 ± 0.0203 | 2.05x ± 0.04 | complete |
| `M0_model` | 1.07 ± 0.00672 | 15.75 ± 0.22 | 14.78x ± 0.09 | complete |
| `Mb_model` | 43.38 ± 0.909 | 53.25 ± 1.79 | 1.24x ± 0.02 | complete |
| `Mh_model` | 15.75 ± 0.618 | 35.59 ± 1.28 | 2.27x ± 0.05 | complete |
| `Mt_model` | 1.149 ± 0.0446 | 18.89 ± 0.364 | 16.49x ± 0.50 | complete |
| `Mtbh_model` | 14.11 ± 0.285 | 43.8 ± 1.51 | 3.17x ± 0.04 | complete |
| `Mth_model` | 25.48 ± 0.337 | 116.8 ± 2.9 | 4.67x ± 0.08 | complete |
| `Rate_1_model` | 0.07797 ± 0.00207 | 0.103 ± 0.00182 | 1.34x ± 0.02 | complete |
| `Rate_2_model` | 0.1359 ± 0.0031 | 0.1861 ± 0.00811 | 1.35x ± 0.04 | complete |
| `Rate_3_model` | 0.1017 ± 0.00511 | 0.1228 ± 0.00212 | 1.20x ± 0.03 | complete |
| `Rate_4_model` | 0.1122 ± 0.00233 | 0.1638 ± 0.00326 | 1.45x ± 0.06 | complete |
| `Rate_5_model` | 0.09647 ± 0.00142 | 0.1272 ± 0.00288 | 1.32x ± 0.02 | complete |
| `Survey_model` | 57.52 ± 0.799 | 65.23 ± 0.701 | 1.13x ± 0.02 | complete |
| `aalto_bern` | 0.07918 ± 0.00238 | 0.1139 ± 0.00183 | 1.44x ± 0.03 | complete |
| `aalto_binom` | 0.07428 ± 0.00205 | 0.1061 ± 0.00309 | 1.39x ± 0.04 | complete |
| `aalto_binom2` | 0.1308 ± 0.00302 | 0.1754 ± 0.00259 | 1.33x ± 0.05 | complete |
| `aalto_binomb` | 0.06222 ± 0.00181 | 0.08661 ± 0.00135 | 1.36x ± 0.05 | complete |
| `aalto_gpareto` | 0.3353 ± 0.00525 | 0.2446 ± 0.00413 | 0.72x ± 0.02 | complete |
| `aalto_grp_aov` | 0.2364 ± 0.00756 | 0.3167 ± 0.00593 | 1.36x ± 0.06 | complete |
| `aalto_grp_prior_mean` | 0.3252 ± 0.00588 | 0.4372 ± 0.0171 | 1.31x ± 0.03 | complete |
| `aalto_grp_prior_mean_var` | 0.5897 ± 0.00514 | 0.7882 ± 0.0191 | 1.33x ± 0.04 | complete |
| `aalto_lin` | 0.1862 ± 0.00478 | 0.3003 ± 0.00474 | 1.62x ± 0.03 | complete |
| `aalto_lin_std` | 0.1859 ± 0.00414 | 0.2933 ± 0.00234 | 1.56x ± 0.05 | complete |
| `aalto_lin_std_t` | 0.2631 ± 0.00653 | 0.3887 ± 0.00941 | 1.49x ± 0.03 | complete |
| `aalto_poisson_hurdle` | 2.288 ± 0.0309 | 58.93 ± 1.92 | 25.73x ± 0.88 | complete |
| `aalto_poisson_simple` | 0.8893 ± 0.0271 | 0.9204 ± 0.0185 | 1.03x ± 0.03 | complete |
| `accel_gp` | 4.737 ± 0.114 | 8.387 ± 0.103 | 1.76x ± 0.07 | complete |
| `accel_splines` | 4.967 ± 0.11 | 7.837 ± 0.17 | 1.57x ± 0.02 | complete |
| `arK` | 1.743 ± 0.0362 | 8.58 ± 0.235 | 4.84x ± 0.13 | complete |
| `arma11` | 4.653 ± 0.194 | 3.877 ± 0.107 | 0.83x ± 0.02 | complete |
| `blr` | 0.5688 ± 0.0302 | 0.4724 ± 0.00667 | 0.83x ± 0.03 | complete |
| `bones_model` | 44.04 ± 0.878 | 58.04 ± 1.02 | 1.31x ± 0.04 | complete |
| `bym2_offset_only` | 37.74 ± 1.17 | 63.81 ± 1.59 | 1.69x ± 0.05 | complete |
| `ch09_m5_8s` | 0.591 ± 0.0109 | 1.199 ± 0.044 | 2.00x ± 0.06 | complete |
| `ch09_m5_8s2` | 0.6041 ± 0.0125 | 1.172 ± 0.00322 | 1.95x ± 0.02 | complete |
| `ch09_m9_1` | 0.9894 ± 0.0379 | 2.038 ± 0.00841 | 2.05x ± 0.08 | complete |
| `ch09_m9_1_chains4` | 0.977 ± 0.0246 | 2.027 ± 0.0453 | 2.10x ± 0.02 | complete |
| `ch09_m9_2` | 0.1117 ± 0.00133 | 0.1613 ± 0.00437 | 1.46x ± 0.03 | complete |
| `ch09_m9_3` | 0.1136 ± 0.00264 | 0.1635 ± 0.00225 | 1.43x ± 0.01 | complete |
| `ch09_m9_4` | 0.2374 ± 0.00424 | 0.3222 ± 0.00531 | 1.33x ± 0.04 | complete |
| `ch09_m9_5` | 0.2414 ± 0.00599 | 0.3122 ± 0.00993 | 1.28x ± 0.02 | complete |
| `ch09_mp` | 0.03982 ± 0.000606 | 0.07738 ± 0.00101 | 2.00x ± 0.04 | complete |
| `ch11_m11_10` | 0.3665 ± 0.0108 | 0.4887 ± 0.00771 | 1.32x ± 0.02 | complete |
| `ch11_m11_11` | 0.4306 ± 0.0145 | 0.7027 ± 0.0128 | 1.64x ± 0.04 | complete |
| `ch11_m11_4` | 6.562 ± 0.0888 | 11.17 ± 0.366 | 1.72x ± 0.02 | complete |
| `ch11_m11_5` | 6.621 ± 0.135 | 14.25 ± 0.146 | 2.19x ± 0.05 | complete |
| `ch11_m11_6` | 0.7414 ± 0.0192 | 0.9474 ± 0.0366 | 1.26x ± 0.01 | complete |
| `ch11_m11_7` | 0.2796 ± 0.0037 | 0.3899 ± 0.00941 | 1.39x ± 0.03 | complete |
| `ch11_m11_8` | 0.4517 ± 0.0154 | 0.5893 ± 0.0106 | 1.32x ± 0.03 | complete |
| `ch11_m11_9` | 0.05121 ± 0.00153 | 0.07942 ± 0.00208 | 1.54x ± 0.04 | complete |
| `ch11_m_pois` | 0.099 ± 0.00303 | 0.1408 ± 0.00373 | 1.44x ± 0.01 | complete |
| `ch12_m12_1` | 1.194 ± 0.0201 | 1.507 ± 0.0282 | 1.25x ± 0.02 | complete |
| `ch12_m12_2` | 0.9366 ± 0.0173 | 1.18 ± 0.0324 | 1.27x ± 0.01 | complete |
| `ch12_m12_3` | 1.085 ± 0.0181 | 18.39 ± 0.169 | 17.19x ± 0.41 | complete |
| `ch12_m12_3_alt` | 3.104 ± 0.0752 | 17.53 ± 0.783 | 5.60x ± 0.26 | complete |
| `ch12_m12_4` | 26.39 ± 0.496 | 2757 ± 38 | 104.92x ± 2.43 | complete |
| `ch12_m12_5` | 2727 ± 93.5 | 3093 ± 48.2 | 1.13x ± 0.01 | complete |
| `ch12_m12_6` | 3037 ± 74.9 | 3092 ± 74.6 | 1.02x ± 0.02 | complete |
| `ch12_m12_7` | 2632 ± 50.9 | 2971 ± 78.7 | 1.12x ± 0.02 | complete |
| `ch13_m13_1` | 0.9533 ± 0.0139 | 1.197 ± 0.0319 | 1.25x ± 0.02 | complete |
| `ch13_m13_2` | 1.05 ± 0.0195 | 1.326 ± 0.0414 | 1.30x ± 0.02 | complete |
| `ch13_m13_3` | 1.219 ± 0.0079 | 1.615 ± 0.0266 | 1.30x ± 0.05 | complete |
| `ch13_m13_4` | 8.577 ± 0.27 | 14.53 ± 0.247 | 1.69x ± 0.06 | complete |
| `ch13_m13_4b` | 8.675 ± 0.348 | 15.04 ± 0.185 | 1.72x ± 0.06 | complete |
| `ch13_m13_4nc` | 8.486 ± 0.132 | 18.29 ± 0.984 | 2.10x ± 0.04 | complete |
| `ch13_m13_5` | 6.473 ± 0.108 | 11.55 ± 0.191 | 1.77x ± 0.04 | complete |
| `ch13_m13_6` | 8.883 ± 0.236 | 15.1 ± 0.415 | 1.69x ± 0.06 | complete |
| `ch13_m13_7` | 0.05356 ± 0.000905 | 0.09961 ± 0.00134 | 1.89x ± 0.03 | complete |
| `ch13_m13_7nc` | 0.03738 ± 0.000689 | 0.07315 ± 0.00186 | 1.99x ± 0.08 | complete |
| `ch14_m14_1` | 6.375 ± 0.171 | 7.327 ± 0.235 | 1.13x ± 0.02 | complete |
| `ch14_m14_10` | 364.1 ± 4.5 | 461.6 ± 16 | 1.28x ± 0.03 | complete |
| `ch14_m14_11` | 547.8 ± 2.95 | 629.1 ± 12.4 | 1.15x ± 0.03 | complete |
| `ch14_m14_2` | 19.4 ± 0.494 | 23.15 ± 0.319 | 1.20x ± 0.02 | complete |
| `ch14_m14_3` | 11.69 ± 0.238 | 20.78 ± 0.439 | 1.83x ± 0.03 | complete |
| `ch14_m14_4` | 1.853 ± 0.0475 | 3.31 ± 0.099 | 1.76x ± 0.05 | complete |
| `ch14_m14_4x` | 1.793 ± 0.0405 | 3.298 ± 0.133 | 1.82x ± 0.02 | complete |
| `ch14_m14_5` | 2.421 ± 0.0496 | 4.974 ± 0.125 | 2.07x ± 0.07 | complete |
| `ch14_m14_6` | 104.6 ± 1.51 | 130.9 ± 2.12 | 1.21x ± 0.02 | complete |
| `ch14_m14_6x` | 101.7 ± 1.35 | 126.2 ± 2.02 | 1.26x ± 0.04 | complete |
| `ch14_m14_7` | 24.46 ± 0.645 | 33.78 ± 0.856 | 1.39x ± 0.03 | complete |
| `ch14_m14_8` | 2.639 ± 0.0499 | 3.482 ± 0.105 | 1.30x ± 0.01 | complete |
| `ch14_m14_8nc` | 2.709 ± 0.0569 | 3.689 ± 0.0713 | 1.37x ± 0.05 | complete |
| `ch14_m14_9` | 345.4 ± 9.86 | 425.8 ± 12.6 | 1.25x ± 0.03 | complete |
| `ch15_m15_1` | 0.6023 ± 0.0182 | 1.116 ± 0.0247 | 1.87x ± 0.04 | complete |
| `ch15_m15_2` | 0.9605 ± 0.0142 | 1.46 ± 0.024 | 1.54x ± 0.03 | complete |
| `ch15_m15_3` | 22.42 ± 0.492 | 21.61 ± 0.777 | 0.97x ± 0.03 | complete |
| `ch15_m15_4` | 18.58 ± 0.792 | 17.07 ± 0.307 | 0.94x ± 0.04 | complete |
| `ch15_m15_5` | 0.5985 ± 0.00595 | 1.296 ± 0.014 | 2.17x ± 0.03 | complete |
| `ch15_m15_6` | 0.3817 ± 0.00497 | 0.6157 ± 0.0085 | 1.62x ± 0.03 | complete |
| `ch15_m15_7` | 10.66 ± 0.133 | 14.34 ± 0.374 | 1.36x ± 0.01 | complete |
| `ch15_m15_8` | 2.965 ± 0.0346 | 5.628 ± 0.163 | 1.89x ± 0.01 | complete |
| `ch15_m15_9` | 3.08 ± 0.101 | 5.737 ± 0.0817 | 1.84x ± 0.04 | complete |
| `ch16_m16_1` | 11.2 ± 0.207 | 25.66 ± 0.252 | 2.34x ± 0.02 | complete |
| `ch16_m16_4` | 1.596 ± 0.0444 | 3.094 ± 0.0564 | 1.94x ± 0.05 | complete |
| `covid19imperial_v2` | 240.8 ± 7.97 | 355.2 ± 6.56 | 1.48x ± 0.08 | complete |
| `covid19imperial_v3` | 246.7 ± 2.47 | 354.6 ± 12.4 | 1.43x ± 0.06 | complete |
| `diamonds` | 33.51 ± 0.0948 | 32.6 ± 1.11 | 0.95x ± 0.02 | complete |
| `dogs` | 6.559 ± 0.163 | 30.93 ± 0.437 | 4.68x ± 0.09 | complete |
| `dogs_hierarchical` | 11.07 ± 0.24 | 35.7 ± 0.78 | 3.22x ± 0.10 | complete |
| `dogs_log` | — | — | — | failed; dogs_log/gradient/0/stanli: failed (event 2269) |
| `dogs_nonhierarchical` | 16.13 ± 0.368 | 41.92 ± 1.26 | 2.62x ± 0.04 | complete |
| `dugongs_model` | 0.547 ± 0.0184 | 0.9782 ± 0.0161 | 1.83x ± 0.08 | complete |
| `earn_height` | 4.341 ± 0.18 | 7.448 ± 0.205 | 1.73x ± 0.02 | complete |
| `eight_schools_centered` | 0.2609 ± 0.00609 | 0.3645 ± 0.00423 | 1.39x ± 0.05 | complete |
| `eight_schools_noncentered` | 0.2288 ± 0.00285 | 0.3133 ± 0.00346 | 1.38x ± 0.01 | complete |
| `election88_full` | 221.1 ± 3.42 | 455.8 ± 3.4 | 2.09x ± 0.01 | complete |
| `extra_hurdle_poisson` | 0.2206 ± 0.00894 | 0.4715 ± 0.0126 | 2.16x ± 0.07 | complete |
| `garch11` | 7.247 ± 0.25 | 6.797 ± 0.17 | 0.94x ± 0.02 | complete |
| `gp_pois_regr` | 2.101 ± 0.0507 | 2.786 ± 0.0724 | 1.33x ± 0.03 | complete |
| `gp_regr` | 2.518 ± 0.0515 | 3.418 ± 0.104 | 1.33x ± 0.03 | complete |
| `gpcm_latent_reg_irt` | 112.9 ± 1.48 | 1765 ± 18.1 | 15.58x ± 0.34 | complete |
| `grsm_latent_reg_irt` | 65.92 ± 1.28 | 929.8 ± 22.1 | 13.73x ± 0.09 | complete |
| `hier_2pl` | 203.7 ± 5.15 | 325.1 ± 15.2 | 1.58x ± 0.04 | complete |
| `hierarchical_gp` | 18.97 ± 0.31 | 44.69 ± 0.601 | 2.36x ± 0.05 | complete |
| `hmm_drive_0` | 122.9 ± 2.09 | 141.9 ± 4.33 | 1.14x ± 0.03 | complete |
| `hmm_drive_1` | 125 ± 2.89 | 152 ± 5.68 | 1.23x ± 0.03 | complete |
| `hmm_example` | 17.8 ± 0.667 | 28.42 ± 1.08 | 1.60x ± 0.06 | complete |
| `hmm_gaussian` | 197.7 ± 2.37 | 275.2 ± 3.73 | 1.39x ± 0.03 | complete |
| `i319_gauss_re` | 4.04 ± 0.115 | 4.759 ± 0.161 | 1.19x ± 0.04 | complete |
| `i319_negbin_fixed` | 8.751 ± 0.141 | 8.384 ± 0.142 | 0.96x ± 0.01 | complete |
| `i319_negbin_re` | 11.09 ± 0.244 | 12.78 ± 0.121 | 1.18x ± 0.03 | complete |
| `i319_pois_fixed` | 2.8 ± 0.0933 | 2.893 ± 0.0562 | 1.00x ± 0.01 | complete |
| `i319_pois_re` | 5.216 ± 0.104 | 6.838 ± 0.218 | 1.33x ± 0.02 | complete |
| `i319_pois_re2` | 7.19 ± 0.175 | 10.34 ± 0.0809 | 1.44x ± 0.03 | complete |
| `i320_gp_expquad` | 11.66 ± 0.128 | 14.6 ± 0.465 | 1.25x ± 0.04 | complete |
| `i320_gp_matern32` | 32.76 ± 1.03 | 28.47 ± 0.845 | 0.87x ± 0.01 | complete |
| `i320_mi_nhanes` | 1.762 ± 0.0382 | 2.131 ± 0.0142 | 1.22x ± 0.02 | complete |
| `i320_pois_trunc_both` | 31.55 ± 0.658 | 30.68 ± 0.621 | 0.99x ± 0.02 | complete |
| `i320_pois_trunc_ub` | 24.44 ± 0.629 | 26.5 ± 0.428 | 1.06x ± 0.01 | complete |
| `i320_sratio_cs` | 81.12 ± 1.38 | 145.1 ± 3.92 | 1.81x ± 0.04 | complete |
| `i320_sratio_plain` | 59.68 ± 1.06 | 72.38 ± 1.29 | 1.20x ± 0.05 | complete |
| `iohmm_reg` | 186 ± 5.8 | 344.1 ± 9.57 | 1.85x ± 0.03 | complete |
| `irt_2pl` | 17.89 ± 0.388 | 22.52 ± 0.523 | 1.26x ± 0.04 | complete |
| `kidscore_interaction` | 2.671 ± 0.0979 | 6.016 ± 0.131 | 2.24x ± 0.07 | complete |
| `kidscore_interaction_c` | 2.645 ± 0.0439 | 6.06 ± 0.243 | 2.29x ± 0.07 | complete |
| `kidscore_interaction_c2` | 2.649 ± 0.0828 | 5.986 ± 0.111 | 2.26x ± 0.07 | complete |
| `kidscore_interaction_z` | 2.622 ± 0.0672 | 6.029 ± 0.115 | 2.26x ± 0.06 | complete |
| `kidscore_mom_work` | 2.65 ± 0.0345 | 5.857 ± 0.201 | 2.20x ± 0.04 | complete |
| `kidscore_momhs` | 1.588 ± 0.0268 | 2.736 ± 0.0553 | 1.71x ± 0.03 | complete |
| `kidscore_momhsiq` | 2.049 ± 0.0153 | 4.427 ± 0.118 | 2.15x ± 0.05 | complete |
| `kidscore_momiq` | 1.6 ± 0.0566 | 2.719 ± 0.0532 | 1.68x ± 0.04 | complete |
| `kilpisjarvi` | 0.3264 ± 0.0114 | 0.5345 ± 0.0172 | 1.63x ± 0.02 | complete |
| `kronecker_gp` | — | — | — | failed; density/gradient mismatch: scaled error 0.0063 |
| `ldaK2` | 50.27 ± 0.847 | 120.9 ± 2.97 | 2.36x ± 0.07 | complete |
| `ldaK5` | 2440 ± 48.8 | 6099 ± 169 | 2.51x ± 0.03 | complete |
| `log10earn_height` | 4.293 ± 0.0602 | 7.774 ± 0.111 | 1.76x ± 0.06 | complete |
| `logearn_height` | 4.348 ± 0.0488 | 7.3 ± 0.136 | 1.66x ± 0.02 | complete |
| `logearn_height_male` | 6.023 ± 0.175 | 12.6 ± 0.445 | 2.09x ± 0.04 | complete |
| `logearn_interaction` | 7.89 ± 0.246 | 16.79 ± 0.625 | 2.18x ± 0.06 | complete |
| `logearn_interaction_z` | 7.677 ± 0.157 | 16.84 ± 0.315 | 2.19x ± 0.03 | complete |
| `logearn_logheight_male` | 5.961 ± 0.143 | 12.46 ± 0.372 | 2.12x ± 0.03 | complete |
| `logistic_regression_rhs` | 42.64 ± 0.749 | 51.85 ± 0.947 | 1.19x ± 0.04 | complete |
| `logmesquite` | 0.4791 ± 0.00709 | 1.382 ± 0.0293 | 2.89x ± 0.04 | complete |
| `logmesquite_logva` | 0.3591 ± 0.003 | 0.8287 ± 0.0199 | 2.29x ± 0.03 | complete |
| `logmesquite_logvas` | 0.478 ± 0.00951 | 1.395 ± 0.0324 | 2.79x ± 0.05 | complete |
| `logmesquite_logvash` | 0.4482 ± 0.00793 | 1.204 ± 0.0339 | 2.68x ± 0.04 | complete |
| `logmesquite_logvolume` | 0.2525 ± 0.00331 | 0.444 ± 0.00977 | 1.76x ± 0.03 | complete |
| `losscurve_sislob` | 1.097 ± 0.0272 | 2.113 ± 0.0799 | 1.92x ± 0.04 | complete |
| `lotka_volterra` | 24.08 ± 0.262 | 47.84 ± 0.475 | 2.02x ± 0.03 | complete |
| `low_dim_gauss_mix` | 51.41 ± 2.26 | 106 ± 2.5 | 2.02x ± 0.02 | complete |
| `low_dim_gauss_mix_collapse` | 48.78 ± 2.17 | 104 ± 1.44 | 2.09x ± 0.07 | complete |
| `lsat_model` | 40.04 ± 1.73 | 64.2 ± 0.98 | 1.62x ± 0.05 | complete |
| `mesquite` | 0.4784 ± 0.0103 | 1.374 ± 0.0113 | 2.89x ± 0.07 | complete |
| `multi_occupancy` | 25.83 ± 0.859 | 59 ± 1.22 | 2.27x ± 0.04 | complete |
| `nes` | 17.09 ± 0.388 | 43.92 ± 0.985 | 2.66x ± 0.05 | complete |
| `nes_logit_model` | 6.575 ± 0.0945 | 6.747 ± 0.214 | 1.03x ± 0.03 | complete |
| `nn_rbm1bJ10` | 162.3 ± 2.21 | 191.6 ± 1.43 | 1.20x ± 0.02 | complete |
| `nn_rbm1bJ100` | 4.287e+05 ± 5.81e+03 | 4.613e+05 ± 938 | 1.07x ± 0.02 | complete |
| `normal_mixture` | 45.46 ± 0.703 | 95.9 ± 3.48 | 2.08x ± 0.05 | complete |
| `normal_mixture_k` | 202.4 ± 4.37 | 395.5 ± 3.8 | 1.93x ± 0.04 | complete |
| `one_comp_mm_elim_abs` | 518.4 ± 11 | 529.9 ± 4.02 | 1.03x ± 0.02 | complete |
| `pilots` | 0.6423 ± 0.0217 | 0.9114 ± 0.0171 | 1.39x ± 0.04 | complete |
| `prophet` | 37.62 ± 1.3 | 56.01 ± 1.77 | 1.49x ± 0.06 | complete |
| `radon_county` | 36.99 ± 0.867 | 77.31 ± 3.08 | 2.10x ± 0.06 | complete |
| `radon_county_intercept` | 54.26 ± 0.75 | 364 ± 10.2 | 6.80x ± 0.06 | complete |
| `radon_hierarchical_intercept_centered` | 68.93 ± 0.834 | 470.3 ± 3.8 | 6.83x ± 0.04 | complete |
| `radon_hierarchical_intercept_noncentered` | 68.85 ± 2.72 | 481.5 ± 8.64 | 6.98x ± 0.20 | complete |
| `radon_partially_pooled_centered` | 37.14 ± 1.05 | 297 ± 3.76 | 7.93x ± 0.17 | complete |
| `radon_partially_pooled_noncentered` | 38.17 ± 1.22 | 285.9 ± 7.41 | 7.66x ± 0.13 | complete |
| `radon_pooled` | 46.16 ± 0.823 | 84.07 ± 2.38 | 1.82x ± 0.05 | complete |
| `radon_variable_intercept_centered` | 53.07 ± 1.37 | 359.2 ± 6.21 | 6.96x ± 0.13 | complete |
| `radon_variable_intercept_noncentered` | 54.35 ± 1.24 | 368.9 ± 4.41 | 6.99x ± 0.22 | complete |
| `radon_variable_intercept_slope_centered` | 62.32 ± 1.17 | 391.1 ± 3.29 | 6.20x ± 0.07 | complete |
| `radon_variable_intercept_slope_noncentered` | 60.13 ± 0.826 | 381.2 ± 9.74 | 6.21x ± 0.06 | complete |
| `radon_variable_slope_centered` | 53.86 ± 0.705 | 370.8 ± 7.88 | 6.86x ± 0.19 | complete |
| `radon_variable_slope_noncentered` | 55.97 ± 1.91 | 368.6 ± 12.1 | 6.63x ± 0.16 | complete |
| `rats_model` | 1.141 ± 0.0129 | 4.683 ± 0.12 | 4.07x ± 0.07 | complete |
| `s2_ar_cov` | 6.03 ± 0.155 | 6.16 ± 0.101 | 1.02x ± 0.01 | complete |
| `s2_beta_binomial` | 4.497 ± 0.116 | 5.05 ± 0.128 | 1.12x ± 0.01 | complete |
| `s2_car` | 0.9621 ± 0.0269 | 1.953 ± 0.025 | 2.00x ± 0.07 | complete |
| `s2_car_esicar` | 0.906 ± 0.0292 | 1.763 ± 0.028 | 1.98x ± 0.02 | complete |
| `s2_car_icar` | 0.7445 ± 0.0121 | 1.741 ± 0.0245 | 2.37x ± 0.05 | complete |
| `s2_categorical_re` | 5.255 ± 0.127 | 7.51 ± 0.116 | 1.47x ± 0.05 | complete |
| `s2_cens_interval` | 1.565 ± 0.037 | 2.754 ± 0.0773 | 1.78x ± 0.06 | complete |
| `s2_com_poisson` | 290.6 ± 13.9 | 48.09 ± 0.535 | 0.17x ± 0.00 | complete |
| `s2_cosy` | 5.608 ± 0.103 | 5.476 ± 0.163 | 0.97x ± 0.01 | complete |
| `s2_cox` | 0.7318 ± 0.00619 | 2.419 ± 0.0481 | 3.24x ± 0.09 | complete |
| `s2_cox_cens` | 0.8096 ± 0.0319 | 3.22 ± 0.0694 | 3.98x ± 0.11 | complete |
| `s2_cumulative_cauchit` | 0.8623 ± 0.00893 | 3.289 ± 0.0949 | 3.90x ± 0.08 | complete |
| `s2_cumulative_cloglog` | 1.132 ± 0.0131 | 2.631 ± 0.0535 | 2.31x ± 0.06 | complete |
| `s2_cumulative_probit` | 4.491 ± 0.071 | 5.899 ± 0.152 | 1.35x ± 0.03 | complete |
| `s2_custom_vint` | 4.657 ± 0.0327 | 6.191 ± 0.178 | 1.37x ± 0.02 | complete |
| `s2_custom_vreal` | 0.353 ± 0.00599 | 1.917 ± 0.0366 | 5.37x ± 0.08 | complete |
| `s2_dirichlet` | 13.45 ± 0.181 | 13.77 ± 0.0515 | 1.05x ± 0.03 | complete |
| `s2_discrete_weibull` | 1.739 ± 0.0529 | 3.597 ± 0.0919 | 2.06x ± 0.07 | complete |
| `s2_dist_sigma_re` | 1.107 ± 0.032 | 2.115 ± 0.0411 | 1.87x ± 0.02 | complete |
| `s2_fcor` | 13.82 ± 0.581 | 13.2 ± 0.352 | 0.95x ± 0.02 | complete |
| `s2_frechet` | 1.96 ± 0.0415 | 2.285 ± 0.0638 | 1.15x ± 0.04 | complete |
| `s2_gev` | 7.267 ± 0.106 | 4.339 ± 0.134 | 0.60x ± 0.01 | complete |
| `s2_gp_approx` | 0.6585 ± 0.0157 | 1.554 ± 0.0128 | 2.33x ± 0.08 | complete |
| `s2_gp_by_approx` | 1.067 ± 0.0379 | 3.552 ± 0.0344 | 3.35x ± 0.04 | complete |
| `s2_gp_by_gr` | — | — | — | failed; density/gradient mismatch: scaled error 5.33e-08 |
| `s2_gr_by` | 1.072 ± 0.0334 | 1.586 ± 0.0196 | 1.49x ± 0.06 | complete |
| `s2_gr_student` | 1.262 ± 0.0492 | 1.913 ± 0.0399 | 1.48x ± 0.01 | complete |
| `s2_hurdle_cumulative` | 6.993 ± 0.0644 | 8.763 ± 0.289 | 1.25x ± 0.01 | complete |
| `s2_hurdle_negbin` | 3.32 ± 0.114 | 7.044 ± 0.153 | 2.12x ± 0.06 | complete |
| `s2_index_mi` | 1.226 ± 0.0356 | 1.58 ± 0.0481 | 1.27x ± 0.02 | complete |
| `s2_invgaussian` | — | — | — | failed; s2_invgaussian/gradient/0/stanli: failed (event 4816) |
| `s2_logistic_normal` | 19.72 ± 0.493 | 20.67 ± 0.197 | 1.06x ± 0.01 | complete |
| `s2_me2` | 2.977 ± 0.0672 | 4.157 ± 0.0547 | 1.39x ± 0.04 | complete |
| `s2_me2_nomecor` | 1.75 ± 0.0118 | 2.897 ± 0.0218 | 1.63x ± 0.04 | complete |
| `s2_mi_lognormal` | 0.849 ± 0.0118 | 1.404 ± 0.0408 | 1.68x ± 0.05 | complete |
| `s2_mi_trunc_lb` | 5.627 ± 0.108 | 5.164 ± 0.153 | 0.92x ± 0.03 | complete |
| `s2_mixture_theta` | 7.021 ± 0.135 | 8.718 ± 0.386 | 1.23x ± 0.03 | complete |
| `s2_mm` | 1.671 ± 0.0311 | 2.055 ± 0.0661 | 1.20x ± 0.04 | complete |
| `s2_mm_weights` | 1.659 ± 0.0398 | 1.984 ± 0.00596 | 1.22x ± 0.01 | complete |
| `s2_mmc` | 3.964 ± 0.0546 | 3.506 ± 0.128 | 0.88x ± 0.02 | complete |
| `s2_mo_simo_prior` | 0.9137 ± 0.0161 | 1.687 ± 0.0499 | 1.85x ± 0.07 | complete |
| `s2_multinomial` | 10.11 ± 0.298 | 11.24 ± 0.217 | 1.13x ± 0.04 | complete |
| `s2_mv_shared_re` | 2.847 ± 0.0339 | 3.227 ± 0.0643 | 1.12x ± 0.03 | complete |
| `s2_mv_subset` | 0.6621 ± 0.0197 | 0.7112 ± 0.0207 | 1.08x ± 0.01 | complete |
| `s2_nl_noloop` | 0.5822 ± 0.00308 | 0.9374 ± 0.00828 | 1.62x ± 0.05 | complete |
| `s2_nlf` | 0.8258 ± 0.0041 | 1.813 ± 0.0288 | 2.23x ± 0.03 | complete |
| `s2_rate` | 0.5309 ± 0.0151 | 0.7588 ± 0.0147 | 1.46x ± 0.01 | complete |
| `s2_s_by` | 1.442 ± 0.0318 | 2.999 ± 0.065 | 2.09x ± 0.05 | complete |
| `s2_s_cc` | 0.4946 ± 0.0153 | 0.9824 ± 0.0385 | 2.03x ± 0.05 | complete |
| `s2_sar` | 3.445 ± 0.123 | 7.832 ± 0.187 | 2.25x ± 0.08 | complete |
| `s2_sar_error` | 3.48 ± 0.083 | 7.841 ± 0.155 | 2.22x ± 0.07 | complete |
| `s2_shifted_lognormal` | 0.5337 ± 0.0107 | 1.075 ± 0.0126 | 2.02x ± 0.03 | complete |
| `s2_t2_by` | 1.617 ± 0.0294 | 2.854 ± 0.0889 | 1.77x ± 0.03 | complete |
| `s2_threading` | 0.3646 ± 0.000771 | 0.4504 ± 0.0118 | 1.19x ± 0.04 | complete |
| `s2_unstr` | 5.954 ± 0.0996 | 6.503 ± 0.135 | 1.10x ± 0.02 | complete |
| `s2_weights_trunc` | 5.905 ± 0.0748 | 6.781 ± 0.152 | 1.15x ± 0.01 | complete |
| `s2_wiener` | 28.12 ± 0.0281 | 29.12 ± 0.486 | 1.03x ± 0.02 | complete |
| `s2_zi_asymlaplace` | 4.895 ± 0.108 | 4.36 ± 0.0511 | 0.89x ± 0.03 | complete |
| `s2_zi_beta` | 4.007 ± 0.123 | 5.838 ± 0.086 | 1.43x ± 0.03 | complete |
| `s2_zoi_beta` | 4.239 ± 0.155 | 6.078 ± 0.122 | 1.41x ± 0.02 | complete |
| `seeds_centered_model` | 0.7856 ± 0.0197 | 1.461 ± 0.0269 | 1.87x ± 0.07 | complete |
| `seeds_model` | 0.7413 ± 0.0113 | 1.163 ± 0.0162 | 1.56x ± 0.05 | complete |
| `seeds_stanified_model` | 0.7764 ± 0.0145 | 1.143 ± 0.033 | 1.44x ± 0.03 | complete |
| `sesame_one_pred_a` | 0.8753 ± 0.0111 | 1.569 ± 0.0363 | 1.80x ± 0.04 | complete |
| `sir` | — | — | — | failed; sir/gradient/0/stanli: failed (event 5547) |
| `soil_incubation` | 30.56 ± 0.408 | 66.46 ± 1.4 | 2.13x ± 0.06 | complete |
| `state_space_stochastic_level_stochastic_seasonal` | 6.663 ± 0.128 | 19.55 ± 0.42 | 3.00x ± 0.06 | complete |
| `surgical_model` | 0.5101 ± 0.0167 | 0.676 ± 0.00321 | 1.30x ± 0.03 | complete |
| `sw_acat` | 48.82 ± 1.78 | 145.4 ± 1.65 | 3.06x ± 0.10 | complete |
| `sw_acat_cs` | 61.06 ± 1.27 | 206.1 ± 2.52 | 3.45x ± 0.07 | complete |
| `sw_ar` | 1.644 ± 0.0445 | 2.546 ± 0.0465 | 1.54x ± 0.04 | complete |
| `sw_arma` | 2.601 ± 0.0573 | 3.578 ± 0.0657 | 1.39x ± 0.04 | complete |
| `sw_asymlaplace` | 4.625 ± 0.103 | 3.576 ± 0.0819 | 0.76x ± 0.02 | complete |
| `sw_bernoulli` | 0.4444 ± 0.00645 | 0.5185 ± 0.00977 | 1.16x ± 0.02 | complete |
| `sw_beta` | 2.026 ± 0.0357 | 2.418 ± 0.0649 | 1.19x ± 0.04 | complete |
| `sw_binomial` | 1.769 ± 0.0507 | 1.994 ± 0.037 | 1.13x ± 0.02 | complete |
| `sw_categorical` | 1.194 ± 0.0376 | 1.351 ± 0.0332 | 1.12x ± 0.01 | complete |
| `sw_cens` | 1.204 ± 0.0314 | 1.887 ± 0.0471 | 1.58x ± 0.05 | complete |
| `sw_cratio` | 59.19 ± 2.46 | 69.97 ± 2.18 | 1.18x ± 0.02 | complete |
| `sw_cratio_cs` | 79.66 ± 2 | 143.7 ± 5.73 | 1.78x ± 0.07 | complete |
| `sw_cumulative` | 21.99 ± 0.158 | 21.12 ± 0.271 | 0.98x ± 0.01 | complete |
| `sw_cumulative_cs` | 51.42 ± 0.987 | 92.72 ± 2.31 | 1.79x ± 0.03 | complete |
| `sw_dist_sigma` | 0.606 ± 0.02 | 1.096 ± 0.0246 | 1.82x ± 0.07 | complete |
| `sw_exgaussian` | 0.9536 ± 0.0167 | 1.257 ± 0.0274 | 1.34x ± 0.06 | complete |
| `sw_gamma` | 0.6815 ± 0.0152 | 0.919 ± 0.00906 | 1.33x ± 0.03 | complete |
| `sw_gaussian` | 0.3718 ± 0.0105 | 0.408 ± 0.0123 | 1.10x ± 0.01 | complete |
| `sw_gp` | 19.89 ± 0.554 | 23.61 ± 0.65 | 1.19x ± 0.03 | complete |
| `sw_hurdle_gamma` | 1.574 ± 0.0511 | 3.467 ± 0.117 | 2.18x ± 0.07 | complete |
| `sw_hurdle_lognormal` | 1.141 ± 0.0024 | 2.734 ± 0.0823 | 2.44x ± 0.06 | complete |
| `sw_hurdle_pois` | 1.615 ± 0.0367 | 3.926 ± 0.0684 | 2.48x ± 0.05 | complete |
| `sw_lognormal` | 0.4183 ± 0.0111 | 0.7093 ± 0.0205 | 1.66x ± 0.02 | complete |
| `sw_ma` | 1.703 ± 0.0409 | 2.603 ± 0.0376 | 1.53x ± 0.03 | complete |
| `sw_me` | 1.071 ± 0.0565 | 1.793 ± 0.0357 | 1.67x ± 0.06 | complete |
| `sw_mi` | 0.8797 ± 0.0149 | 0.9652 ± 0.0197 | 1.09x ± 0.01 | complete |
| `sw_mixture` | 5.194 ± 0.0905 | 7.185 ± 0.155 | 1.41x ± 0.04 | complete |
| `sw_mono` | 0.9121 ± 0.034 | 1.71 ± 0.00604 | 1.86x ± 0.04 | complete |
| `sw_mv_norescor` | 0.6863 ± 0.00811 | 0.7398 ± 0.0216 | 1.08x ± 0.04 | complete |
| `sw_mv_rescor` | 6.766 ± 0.155 | 7.899 ± 0.161 | 1.13x ± 0.02 | complete |
| `sw_negbinomial` | 1.651 ± 0.0272 | 1.684 ± 0.0513 | 1.03x ± 0.02 | complete |
| `sw_nonlinear` | 0.5431 ± 0.0117 | 0.9709 ± 0.032 | 1.74x ± 0.01 | complete |
| `sw_poisson` | 0.5885 ± 0.00995 | 0.6604 ± 0.0131 | 1.10x ± 0.03 | complete |
| `sw_re_bern` | 0.9688 ± 0.0419 | 1.563 ± 0.0466 | 1.61x ± 0.04 | complete |
| `sw_re_gauss` | 1.007 ± 0.0196 | 1.425 ± 0.0358 | 1.41x ± 0.02 | complete |
| `sw_re_negbin` | 2.156 ± 0.057 | 2.833 ± 0.0474 | 1.32x ± 0.02 | complete |
| `sw_re_pois` | 1.082 ± 0.0538 | 1.677 ± 0.039 | 1.58x ± 0.05 | complete |
| `sw_re_slope` | 2.362 ± 0.0549 | 2.521 ± 0.0527 | 1.06x ± 0.02 | complete |
| `sw_se` | 0.3545 ± 0.0123 | 0.5731 ± 0.0188 | 1.66x ± 0.04 | complete |
| `sw_skewnormal` | 1.477 ± 0.055 | 2.097 ± 0.033 | 1.39x ± 0.02 | complete |
| `sw_spline_s` | 0.563 ± 0.00888 | 1.157 ± 0.0221 | 2.08x ± 0.08 | complete |
| `sw_spline_t2` | 0.9859 ± 0.0183 | 1.74 ± 0.049 | 1.77x ± 0.07 | complete |
| `sw_sratio` | 59.04 ± 2.33 | 68.93 ± 1.47 | 1.17x ± 0.02 | complete |
| `sw_student` | 0.5264 ± 0.0116 | 0.9251 ± 0.0186 | 1.75x ± 0.03 | complete |
| `sw_trunc` | 5.898 ± 0.101 | 6.512 ± 0.274 | 1.12x ± 0.03 | complete |
| `sw_vonmises` | 0.6966 ± 0.0298 | 1.568 ± 0.0215 | 2.26x ± 0.05 | complete |
| `sw_weibull` | 1.946 ± 0.0605 | 2.238 ± 0.0316 | 1.13x ± 0.02 | complete |
| `sw_weights` | 0.5516 ± 0.0117 | 1.798 ± 0.047 | 3.25x ± 0.05 | complete |
| `sw_zi_binomial` | 2.031 ± 0.0672 | 3.591 ± 0.0859 | 1.77x ± 0.04 | complete |
| `sw_zi_negbin` | 2.901 ± 0.0878 | 4.541 ± 0.129 | 1.57x ± 0.02 | complete |
| `sw_zi_poisson` | 1.103 ± 0.0274 | 2.219 ± 0.02 | 2.03x ± 0.04 | complete |
| `wells_daae_c_model` | 20.67 ± 0.21 | 21.55 ± 0.916 | 1.06x ± 0.02 | complete |
| `wells_dae_c_model` | 19.44 ± 0.577 | 19.94 ± 0.407 | 1.03x ± 0.03 | complete |
| `wells_dae_inter_model` | 21.88 ± 0.336 | 21.06 ± 0.41 | 1.00x ± 0.01 | complete |
| `wells_dae_model` | 20.1 ± 0.383 | 20.76 ± 0.764 | 1.05x ± 0.03 | complete |
| `wells_dist` | 21.95 ± 0.617 | 33.78 ± 0.781 | 1.53x ± 0.02 | complete |
| `wells_dist100_model` | 17.61 ± 0.158 | 17.57 ± 0.6 | 1.01x ± 0.01 | complete |
| `wells_dist100ars_model` | 18.84 ± 0.571 | 19.08 ± 0.228 | 1.02x ± 0.03 | complete |
| `wells_interaction_c_model` | 20.42 ± 0.186 | 21.38 ± 0.434 | 1.04x ± 0.03 | complete |
| `wells_interaction_model` | 20.82 ± 0.59 | 20.62 ± 0.454 | 0.98x ± 0.02 | complete |
<!--/gen-->

</details>
