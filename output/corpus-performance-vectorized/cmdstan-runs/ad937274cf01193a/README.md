# Corpus benchmark evidence

Run `ad937274cf01193a`, started 2026-10-16T00:24:02Z, records 1 application models.

The manifest identifies the exact sources, inputs, binaries and settings. The summary, raw paired observations and command events are retained here. The summary retains the protocol's setup-plus-20,000-gradient estimate; the table below recalculates totals for 2,000 gradients from the same measurements. Full sampling is not run. Failures remain in the inventory.

Full inputs, generated code and command logs remain locally at `/Users/xitrium/claud/corpus-bench/corpus-vec-supp-ctsem_ctsm_400-2026-10-15.tsv.run`; these larger artifacts are not hosted with this report.

Run `ad937274cf01193a` (2026-10-16): 1 models, 6 alternating gradient pairs.

| Model | CmdStan / Stanli | Stanli total<br>sampling (s) | CmdStan total<br>sampling (s) |
| --- | ---: | ---: | ---: |
| `ctsem_ctsm_400` | 2.05x | 21.9 | 501 |

Total sampling is an estimate: each engine's measured setup time + 2,000 × its median warm gradient time. Full sampling is not run.
