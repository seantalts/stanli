# Rethinking models: speedup and numerical differences

16 September 2026

We compared the unchanged Stan programs and processed data from all 61 ulam() call sites in the second-edition book supplement. Every call site completed four sampling seeds in both engines. A separate hurdle example is also included.

The median end-to-end sampling speedup was 1.58x (CmdStan time / Stanli time). All 61 book call sites had a ratio above 1. Stanli preparation is included; the CmdStan model was already compiled. A value of 2x means Stanli took half as long.

## Numerical differences

At three fixed parameter vectors per fixture, we compared 32,349 values across 62 fixtures. The largest absolute differences were 1.88e-09 for log density, 1.86e-09 for a gradient component, and 3.55e-15 for a model output. The tables give the model-specific maxima.

## How to read the results

Sampling speedup = median CmdStan CLI time / median Stanli CLI time, using four seeds per engine. Each run has 1,000 warmup iterations and 1,000 retained draws. Stanli time includes Stan translation, preparation, warmup, sampling, generated quantities and CSV output. CmdStan time starts from its compiled model. The ratio describes this fixed sampling budget; it does not compare time to an equal effective sample size.

Absolute difference = |Stanli value - CmdStan value|. Log density includes the parameter-transform Jacobian. The gradient column takes the maximum over all unconstrained parameter derivatives; the output column covers constrained parameters, transformed parameters and generated quantities. Each maximum spans all three fixed parameter vectors. These are comparisons at identical parameter values, not differences between random posterior draws.

The supporting CSV also reports ULP distance: the number of representable double-precision steps between the two values. ULP distance depends on the scale of the values and can be large near zero despite a tiny absolute difference. The report therefore uses absolute differences; zero means the compared values were equal.

## Measurement conditions

Apple M3 Ultra, 96 GiB RAM, macOS ARM64; Release build; one engine at a time. Rethinking 2.42, CmdStan 2.39.0 and posterior 1.7.0. Runtime/compiler revision 6e462c2e; run fd5e0ecacddc7047. The toolchain was already installed. The manifest records executable, dependency and input identities.

Seeds 1-4; target acceptance 0.8; maximum tree depth 10; random initialization. Every model used the same iteration budget. The book's model-specific tuning and chain-count choices were not reproduced. CmdStan ran first for each seed. Stanli had a limit of three times that CmdStan CLI duration or 900 seconds, whichever was smaller; CmdStan also had a 900-second limit. All book runs completed. Sampling order was not counterbalanced.

Numerical comparisons use the separately recorded high-precision CmdStan values and the frozen Stanli checker from the timing build. The retained per-seed CSVs use the CLI defaults: eight significant digits for CmdStan and 17 for Stanli. The supporting CSV lists divergence counts, maximum R-hat and minimum bulk ESS for each engine.

## Estimated first-fit speedup

Including the separately measured CmdStan translation and C++ build gives a median estimated first-fit speedup of 37.32x across the 61 book call sites. These estimates add measured stages; they are not directly timed combined runs. Installation is excluded. The main tables use the already-compiled CmdStan comparison.

## Full speedup and numerics appendix

Speedup: CmdStan/Stanli median complete CLI time. Error columns: maximum absolute difference across three parameter vectors. The full-precision values, per-seed times, ULP distances and sampler measurements are retained in the accompanying CSV and JSON files.

| Book model | R code | Sampling speedup | Log density max abs. difference | Gradient max abs. difference | Output max abs. difference |
| --- | --- | ---: | ---: | ---: | ---: |
| m9.1 | 9.14 | 1.82x | 0 | 2.84e-14 | 0 |
| m9.1 | 9.16 | 1.84x | 0 | 2.84e-14 | 0 |
| m9.2 | 9.22 | 1.41x | 0 | 0 | 0 |
| m9.3 | 9.24 | 1.35x | 0 | 0 | 0 |
| m9.4 | 9.26 | 1.45x | 0 | 0 | 0 |
| m9.5 | 9.27 | 1.41x | 0 | 0 | 0 |
| mp | 9.28 | 3.12x | 0 | 0 | 0 |
| m5.8s | 9.29 | 2.89x | 0 | 2.73e-12 | 0 |
| m5.8s2 | 9.30 | 3.01x | 0 | 1.36e-12 | 0 |
| m11.4 | 11.11 | 1.81x | 0 | 3.20e-14 | 0 |
| m11.5 | 11.19 | 2.11x | 0 | 1.03e-13 | 0 |
| m11.6 | 11.25 | 1.42x | 0 | 0 | 0 |
| m11.7 | 11.29 | 1.27x | 0 | 1.14e-13 | 0 |
| m11.8 | 11.32 | 1.51x | 0 | 0 | 0 |
| m11.9 | 11.45 | 1.26x | 0 | 0 | 0 |
| m11.10 | 11.45 | 1.36x | 0 | 0 | 0 |
| m11.11 | 11.49 | 1.62x | 0 | 1.86e-09 | 0 |
| m_pois | 11.61 | 1.14x | 0 | 0 | 0 |
| m12.1 | 12.2 | 1.28x | 0 | 8.88e-16 | 0 |
| m12.2 | 12.6 | 1.41x | 1.42e-14 | 7.11e-15 | 0 |
| m12.3 | 12.9 | 9.28x | 1.99e-12 | 5.54e-13 | 0 |
| m12.3_alt | 12.11 | 4.42x | 1.19e-12 | 9.95e-13 | 0 |
| m12.4 | 12.16 | 146.87x | 1.88e-09 | 1.42e-10 | 0 |
| m12.5 | 12.24 | 1.58x | 5.86e-10 | 1.00e-11 | 0 |
| m12.6 | 12.34 | 1.45x | 8.37e-10 | 3.50e-12 | 0 |
| m12.7 | 12.37 | 1.60x | 8.44e-10 | 7.19e-11 | 0 |
| m13.1 | 13.2 | 1.49x | 0 | 0 | 0 |
| m13.2 | 13.3 | 1.49x | 0 | 0 | 0 |
| m13.3 | 13.13 | 1.12x | 0 | 0 | 0 |
| m13.4 | 13.21 | 1.61x | 0 | 2.84e-14 | 0 |
| m13.5 | 13.23 | 1.79x | 0 | 4.97e-14 | 0 |
| m13.6 | 13.25 | 2.19x | 0 | 2.84e-14 | 0 |
| m13.7 | 13.26 | 1.32x | 0 | 0 | 0 |
| m13.7nc | 13.27 | 1.28x | 0 | 0 | 0 |
| m13.4b | 13.28 | 1.47x | 0 | 2.84e-14 | 0 |
| m13.4nc | 13.29 | 2.62x | 0 | 2.93e-14 | 5.55e-17 |
| m14.1 | 14.12 | 1.40x | 0 | 1.07e-14 | 0 |
| m14.2 | 14.18 | 1.46x | 5.68e-14 | 1.07e-14 | 0 |
| m14.3 | 14.19 | 1.76x | 5.68e-14 | 1.07e-14 | 0 |
| m14.4 | 14.24 | 2.60x | 0 | 1.42e-13 | 0 |
| m14.5 | 14.25 | 3.31x | 0 | 8.53e-14 | 0 |
| m14.6 | 14.26 | 1.71x | 1.14e-13 | 2.84e-13 | 0 |
| m14.4x | 14.27 | 2.61x | 0 | 1.42e-13 | 0 |
| m14.6x | 14.27 | 1.74x | 1.14e-13 | 2.84e-13 | 0 |
| m14.7 | 14.31 | 1.45x | 2.27e-13 | 5.91e-12 | 0 |
| m14.8 | 14.39 | 1.51x | 5.82e-11 | 9.31e-10 | 0 |
| m14.8nc | 14.46 | 1.44x | 0 | 1.86e-09 | 0 |
| m14.9 | 14.49 | 1.22x | 0 | 5.68e-14 | 0 |
| m14.10 | 14.51 | 1.24x | 1.42e-14 | 6.39e-14 | 0 |
| m14.11 | 14.52 | 1.17x | 0 | 3.21e-12 | 0 |
| m15.1 | 15.3 | 1.94x | 0 | 1.78e-15 | 0 |
| m15.2 | 15.5 | 1.79x | 1.14e-13 | 2.22e-16 | 0 |
| m15.3 | 15.12 | 1.15x | 0 | 0 | 0 |
| m15.4 | 15.13 | 1.32x | 0 | 0 | 0 |
| m15.5 | 15.17 | 1.76x | 0 | 3.55e-15 | 0 |
| m15.6 | 15.19 | 1.44x | 0 | 1.78e-15 | 0 |
| m15.7 | 15.22 | 1.65x | 0 | 4.00e-15 | 0 |
| m15.8 | 15.30 | 2.00x | 1.99e-13 | 5.68e-14 | 0 |
| m15.9 | 15.31 | 2.15x | 1.99e-13 | 1.71e-13 | 3.55e-15 |
| m16.1 | 16.2 | 2.35x | 1.14e-13 | 1.71e-13 | 0 |
| m16.4 | 16.11 | 2.83x | 0 | 1.71e-13 | 0 |

## Supplemental fixture (not a book model)

Hurdle Poisson: 1.29x sampling speedup; maximum absolute differences 3.55e-15 (log density), 8.88e-16 (gradient), 0 (output).

## Scope and reproducibility

All 61 ulam() call sites in the chapters 4-16 supplement are listed, representing 58 distinct Stan/data pairs. Repeated fits remain listed. The separate hurdle example brings the total to 62 fixtures. Chapters 4-8 and 10 have no ulam() calls in this supplement. quap(), direct stan() calls, exercises and lecture-only models are outside this inventory.

[Pinned book supplement](https://github.com/rmcelreath/rethinking/blob/ac1b3b2cda83f3e14096e2d997a6e30ad109eeee/book_code_boxes.txt). The generator preserves the Stan text and processed data. Simulation seeds and data licensing are recorded in tests/rethinking/PROVENANCE.md. Both engines receive identical input bytes.

See `benchmark-manifest.json`, `rethinking-results.json`, `rethinking-timings.csv`, `numerical-errors.json` and `numerical-values.json.gz`. The last file retains every compared value from both engines. Raw sampling CSVs and command logs remain in the original run directory.
