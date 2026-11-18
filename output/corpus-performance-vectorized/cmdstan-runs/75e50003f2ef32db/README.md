# Corpus benchmark evidence

Run `75e50003f2ef32db`, started 2026-11-18T07:05:10Z, records 1 application models.

The manifest identifies the exact sources, inputs, binaries and settings. The summary, raw paired observations and command events are retained here. The table below gives each model's setup time plus its gradient count times its measured gradient time. Full sampling is not run. Failures remain in the inventory.

Full inputs, generated code and command logs remain locally at `/private/tmp/claude-501/-Users-xitrium-claud/648e2718-910e-40df-aa10-c6b15aecd8c0/scratchpad/refresh028/vec/rbioacc_/vectorized.tsv.run`; these larger artifacts are not hosted with this report.

Run `75e50003f2ef32db` (2026-11-18): 1 models, 6 alternating gradient pairs.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling |
| --- | ---: | ---: | ---: | ---: | ---: |
| `rbioacc_odetk` | 0.828x | 20.4k | 5.54 | 9.89 | 1.78x |

Total sampling is an estimate: each engine's measured setup time + the number of gradients a 1,000 warmup + 1,000 draw run takes (measured once per model) × its warm gradient time, averaged over 10 points of that run. The same gradient count is used for both engines. Full sampling is not run.
