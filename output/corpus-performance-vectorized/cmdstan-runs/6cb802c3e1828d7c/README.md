# Corpus benchmark evidence

Run `6cb802c3e1828d7c`, started 2026-11-18T07:06:02Z, records 4 application models.

The manifest identifies the exact sources, inputs, binaries and settings. The summary, raw paired observations and command events are retained here. The table below gives each model's setup time plus its gradient count times its measured gradient time. Full sampling is not run. Failures remain in the inventory.

Full inputs, generated code and command logs remain locally at `/private/tmp/claude-501/-Users-xitrium-claud/648e2718-910e-40df-aa10-c6b15aecd8c0/scratchpad/refresh028/vec/rstanarm_/vectorized.tsv.run`; these larger artifacts are not hosted with this report.

Run `6cb802c3e1828d7c` (2026-11-18): 4 models, 6 alternating gradient pairs.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling |
| --- | ---: | ---: | ---: | ---: | ---: |
| `rstanarm_glm_gaussian` | 1.82x | 14.9k | 0.261 | 34.2 | 131x |
| `rstanarm_glmer_nb_count` | 1.39x | 221k | 1.73 | 22.3 | 12.9x |
| `rstanarm_polr` | 0.983x | 15.2k | 0.226 | 11.1 | 49x |
| `rstanarm_glmer_bernoulli` | 0.845x | 57.3k | 1.26 | 19.9 | 15.8x |

Total sampling is an estimate: each engine's measured setup time + the number of gradients a 1,000 warmup + 1,000 draw run takes (measured once per model) × its warm gradient time, averaged over 10 points of that run. The same gradient count is used for both engines. Full sampling is not run.
