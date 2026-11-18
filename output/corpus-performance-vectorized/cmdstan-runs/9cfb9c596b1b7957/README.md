# Corpus benchmark evidence

Run `9cfb9c596b1b7957`, started 2026-11-18T06:32:14Z, records 2 application models.

The manifest identifies the exact sources, inputs, binaries and settings. The summary, raw paired observations and command events are retained here. The table below gives each model's setup time plus its gradient count times its measured gradient time. Full sampling is not run. Failures remain in the inventory.

Full inputs, generated code and command logs remain locally at `/private/tmp/claude-501/-Users-xitrium-claud/648e2718-910e-40df-aa10-c6b15aecd8c0/scratchpad/refresh028/vec/hdbayes_/vectorized.tsv.run`; these larger artifacts are not hosted with this report.

Run `9cfb9c596b1b7957` (2026-11-18): 2 models, 6 alternating gradient pairs.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling |
| --- | ---: | ---: | ---: | ---: | ---: |
| `hdbayes_lm_npp` | 0.889x | 88.7k | 0.231 | 6.54 | 28.3x |

Total sampling is an estimate: each engine's measured setup time + the number of gradients a 1,000 warmup + 1,000 draw run takes (measured once per model) × its warm gradient time, averaged over 10 points of that run. The same gradient count is used for both engines. Full sampling is not run.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling | Reason |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `hdbayes_pwe_npp` | — | — | — | — | — | failed; hdbayes_pwe_npp/gradient/0/cmdstan: failed (event 27) |
