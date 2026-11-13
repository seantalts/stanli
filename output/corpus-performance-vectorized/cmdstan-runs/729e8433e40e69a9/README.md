# Corpus benchmark evidence

Run `729e8433e40e69a9`, started 2026-11-15T00:21:25Z, records 7 application models.

The manifest identifies the exact sources, inputs, binaries and settings. The summary, raw paired observations and command events are retained here. The table below gives each model's setup time plus its gradient count times its measured gradient time. Full sampling is not run. Failures remain in the inventory.

Full inputs, generated code and command logs remain locally at `/private/tmp/claude-501/-Users-xitrium-claud/464f5da1-aa71-4efb-be11-cd1664cf85f7/scratchpad/bench-publish/vecf-i320.tsv.run`; these larger artifacts are not hosted with this report.

Run `729e8433e40e69a9` (2026-11-15): 7 models, 6 alternating gradient pairs.

CmdStan stanc and build times for 7 of 7 models were reused from the build cache and not remeasured in this run.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling |
| --- | ---: | ---: | ---: | ---: | ---: |
| `i320_mi_nhanes` | 2.34x | 312k | 0.279 | 3.85 | 13.8x |
| `i320_sratio_cs` | 1.61x | 20.4k | 1.69 | 6.8 | 4.02x |
| `i320_gp_expquad` | 1.36x | 52.4k | 0.57 | 6.87 | 12.1x |
| `i320_sratio_plain` | 1.11x | 15.4k | 0.927 | 3.86 | 4.17x |
| `i320_pois_trunc_ub` | 0.995x | 21.9k | 0.581 | 2.8 | 4.82x |
| `i320_pois_trunc_both` | 0.973x | 21.9k | 0.735 | 2.99 | 4.07x |
| `i320_gp_matern32` | 0.931x | 133k | 4.19 | 9.85 | 2.35x |

Total sampling is an estimate: each engine's measured setup time + the number of gradients a 1,000 warmup + 1,000 draw run takes (measured once per model) × its warm gradient time, averaged over 10 points of that run. The same gradient count is used for both engines. Full sampling is not run.
