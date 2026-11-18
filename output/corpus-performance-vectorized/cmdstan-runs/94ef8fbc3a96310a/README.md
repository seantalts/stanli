# Corpus benchmark evidence

Run `94ef8fbc3a96310a`, started 2026-11-18T06:26:08Z, records 2 application models.

The manifest identifies the exact sources, inputs, binaries and settings. The summary, raw paired observations and command events are retained here. The table below gives each model's setup time plus its gradient count times its measured gradient time. Full sampling is not run. Failures remain in the inventory.

Full inputs, generated code and command logs remain locally at `/private/tmp/claude-501/-Users-xitrium-claud/648e2718-910e-40df-aa10-c6b15aecd8c0/scratchpad/refresh028/vec/epinow2_estimate_/vectorized.tsv.run`; these larger artifacts are not hosted with this report.

Run `94ef8fbc3a96310a` (2026-11-18): 2 models, 6 alternating gradient pairs.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling |
| --- | ---: | ---: | ---: | ---: | ---: |
| <code>epinow2_estimate_</code><br><code>infections</code> | 1.2x | 87.8k | 3.57 | 159 | 44.5x |
| <code>epinow2_estimate_</code><br><code>secondary</code> | 1.15x | 42.6k | 0.803 | 74.8 | 93.1x |

Total sampling is an estimate: each engine's measured setup time + the number of gradients a 1,000 warmup + 1,000 draw run takes (measured once per model) × its warm gradient time, averaged over 10 points of that run. The same gradient count is used for both engines. Full sampling is not run.
