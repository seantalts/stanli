# Corpus benchmark evidence

Run `68c1ac87a5835bac`, started 2026-11-15T00:37:08Z, records 5 application models.

The manifest identifies the exact sources, inputs, binaries and settings. The summary, raw paired observations and command events are retained here. The table below gives each model's setup time plus its gradient count times its measured gradient time. Full sampling is not run. Failures remain in the inventory.

Full inputs, generated code and command logs remain locally at `/private/tmp/claude-501/-Users-xitrium-claud/464f5da1-aa71-4efb-be11-cd1664cf85f7/scratchpad/bench-publish/vecf-s2_cu.tsv.run`; these larger artifacts are not hosted with this report.

Run `68c1ac87a5835bac` (2026-11-15): 5 models, 6 alternating gradient pairs.

CmdStan stanc and build times for 5 of 5 models were reused from the build cache and not remeasured in this run.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling |
| --- | ---: | ---: | ---: | ---: | ---: |
| `s2_custom_vreal` | 7.44x | 10.2k | 0.0115 | 2.24 | 195x |
| `s2_cumulative_cauchit` | 3.85x | 20.7k | 0.0285 | 2.61 | 91.4x |
| `s2_cumulative_cloglog` | 2.23x | 20.3k | 0.0338 | 2.62 | 77.4x |
| `s2_cumulative_probit` | 1.4x | 13.7k | 0.0654 | 2.54 | 38.8x |
| `s2_custom_vint` | 1.18x | 10.5k | 0.0593 | 2.37 | 40x |

Total sampling is an estimate: each engine's measured setup time + the number of gradients a 1,000 warmup + 1,000 draw run takes (measured once per model) × its warm gradient time, averaged over 10 points of that run. The same gradient count is used for both engines. Full sampling is not run.
