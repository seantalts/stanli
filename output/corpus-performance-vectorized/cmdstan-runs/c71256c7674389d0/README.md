# Corpus benchmark evidence

Run `c71256c7674389d0`, started 2026-11-18T06:25:05Z, records 2 application models.

The manifest identifies the exact sources, inputs, binaries and settings. The summary, raw paired observations and command events are retained here. The table below gives each model's setup time plus its gradient count times its measured gradient time. Full sampling is not run. Failures remain in the inventory.

Full inputs, generated code and command logs remain locally at `/private/tmp/claude-501/-Users-xitrium-claud/648e2718-910e-40df-aa10-c6b15aecd8c0/scratchpad/refresh028/vec/ecoensemble_/vectorized.tsv.run`; these larger artifacts are not hosted with this report.

Run `c71256c7674389d0` (2026-11-18): 2 models, 6 alternating gradient pairs.

CmdStan stanc and build times for 1 of 2 models were reused from the build cache and not remeasured in this run.

| Model | CmdStan / Stanli<br>gradient | Gradients | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | CmdStan / Stanli<br>total sampling |
| --- | ---: | ---: | ---: | ---: | ---: |
| <code>ecoensemble_</code><br><code>hierarchical_withdrivers</code> | 2.92x | 163k | 15 | 67 | 4.46x |
| <code>ecoensemble_</code><br><code>hierarchical_</code><br><code>withdrivers_conj</code> | 2.86x | 192k | 17.8 | 73.9 | 4.15x |

Total sampling is an estimate: each engine's measured setup time + the number of gradients a 1,000 warmup + 1,000 draw run takes (measured once per model) × its warm gradient time, averaged over 10 points of that run. The same gradient count is used for both engines. Full sampling is not run.
