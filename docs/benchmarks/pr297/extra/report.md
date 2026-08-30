# Runtime before/after benchmark

Before: `917c634170420071f76fb6fb90a5c47fd708c7ab`. After: `8d8b37f79dd9b5c5e408a33321664fd86a5d4f36`.

All ratios are after / before; lower is faster. Timings are medians; raw paired samples and quartiles are in results.json. Numerical ULPs cover LP and every gradient at all requested points; failures are not dropped.

| Model | Before grad (µs) | After grad (µs) | Ratio | Before prep (ms) | After prep (ms) | Before RSS (MiB) | After RSS (MiB) | Max ULP | Status |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `canary_pr236_island` | 0.259 | 0.199 | 0.770 | 0.614 | 0.654 | 4.375 | 4.375 | 0 | measured |
| `canary_cfg_native_large_structured` | 297.257 | 140.267 | 0.472 | 0.935 | 2.316 | 9.000 | 11.109 | 173 | >2 ULP; investigate |
| `canary_scan_prepared_solve_retention` | 60671.333 | 71292.167 | 1.175 | 60.485 | 4.013 | 280.391 | 15.000 | 0 | >5% slower; confirm |
| `canary_state_space` | 6.503 | 10.424 | 1.603 | 2.499 | 3.018 | 6.562 | 6.516 | 8 | >5% slower; confirm; >2 ULP; investigate |
| `ctsem_N32_default` | — | — | — | — | — | — | — | — | no_joint_finite_point |
| `ctsem_N32_experimental` | — | — | — | — | — | — | — | — | no_joint_finite_point |
| `ctsem_N33_default` | — | — | — | — | — | — | — | — | no_joint_finite_point |
| `ctsem_N33_experimental` | — | — | — | — | — | — | — | — | no_joint_finite_point |
