# Corpus benchmark evidence

Run `532c791f4c877dc4`, started 2026-10-16T00:23:37Z, records 1 application models.

The manifest identifies the exact sources, inputs, binaries and settings. The summary, raw paired observations and command events are retained here. The summary retains the protocol's setup-plus-20,000-gradient estimate; the table below recalculates totals for 5,000 gradients from the same measurements. Full sampling is not run. Failures remain in the inventory.

Full inputs, generated code and command logs remain locally at `/Users/xitrium/claud/corpus-bench/corpus-vec-supp-s2_gp_by_gr-2026-10-15.tsv.run`; these larger artifacts are not hosted with this report.

Run `532c791f4c877dc4` (2026-10-16): 1 models, 6 alternating gradient pairs.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) |
| --- | ---: | ---: | ---: |

Total sampling is an estimate: each engine's measured setup time + 5,000 × its median warm gradient time. Full sampling is not run.

**Incomplete results**

Missing measurements are —; available timings are retained.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) | Reason |
| --- | ---: | ---: | ---: | --- |
| `s2_gp_by_gr` | — | — | — | failed; density/gradient mismatch: scaled error 5.33e-08 |
