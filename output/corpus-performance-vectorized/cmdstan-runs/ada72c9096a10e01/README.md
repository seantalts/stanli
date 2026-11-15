# Corpus benchmark evidence

Run `ada72c9096a10e01`, started 2026-11-15T00:10:50Z, records 6 application models.

The manifest identifies the exact sources, inputs, binaries and settings. The summary, raw paired observations and command events are retained here. The table below gives each model's setup time plus its gradient count times its measured gradient time. Full sampling is not run. Failures remain in the inventory.

Full inputs, generated code and command logs remain locally at `/private/tmp/claude-501/-Users-xitrium-claud/464f5da1-aa71-4efb-be11-cd1664cf85f7/scratchpad/bench-publish/vecf-i319.tsv.run`; these larger artifacts are not hosted with this report.

Run `ada72c9096a10e01` (2026-11-15): 6 models, 6 alternating gradient pairs.

CmdStan stanc and build times for 6 of 6 models were reused from the build cache and not remeasured in this run.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling |
| --- | ---: | ---: | ---: | ---: | ---: |
| `i319_gauss_re` | 3.17x | 41.3k | 0.108 | 3.59 | 33.3x |
| `i319_pois_re2` | 2.22x | 173k | 0.836 | 5.47 | 6.54x |
| `i319_pois_re` | 1.85x | 192k | 0.72 | 4.65 | 6.46x |
| `i319_negbin_re` | 1.34x | 66.7k | 0.66 | 4.46 | 6.76x |
| `i319_pois_fixed` | 1.03x | 24k | 0.0734 | 2 | 27.2x |
| `i319_negbin_fixed` | 1.02x | 19.4k | 0.187 | 2.37 | 12.7x |

Total sampling is an estimate: each engine's measured setup time + the number of gradients a 1,000 warmup + 1,000 draw run takes (measured once per model) × its warm gradient time, averaged over 10 points of that run. The same gradient count is used for both engines. Full sampling is not run.
