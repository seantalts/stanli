# Runtime before/after benchmark

Before: `917c634170420071f76fb6fb90a5c47fd708c7ab`. After: `8d8b37f79dd9b5c5e408a33321664fd86a5d4f36`.

All ratios are after / before; lower is faster. Timings are medians; raw paired samples and quartiles are in results.json. Numerical ULPs cover LP and every gradient at all requested points; failures are not dropped.

| Model | Before grad (µs) | After grad (µs) | Ratio | Before prep (ms) | After prep (ms) | Before RSS (MiB) | After RSS (MiB) | Max ULP | Status |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `diagnostic_state_space_no_scan` | 6.727 | 6.893 | 1.019 | 2.703 | 2.775 | 6.625 | 6.844 | 0 | measured |
| `diagnostic_prepared_solve_no_scan` | 62484.025 | 62999.192 | 1.008 | 62.027 | 398.853 | 280.297 | 942.844 | 0 | measured |
| `diagnostic_garch11_no_island` | 9.201 | 8.550 | 0.936 | 1.846 | 1.764 | 6.172 | 6.297 | 0 | measured |
| `diagnostic_garch11_no_native_adj` | 12.921 | 13.677 | 1.051 | 2.140 | 2.261 | 6.406 | 6.594 | 0 | >5% slower; confirm |
| `diagnostic_accel_splines_no_island` | 7.563 | 7.510 | 0.986 | 3.960 | 3.809 | 7.984 | 8.109 | 0 | measured |
| `diagnostic_accel_splines_no_native_adj` | 8.469 | 8.947 | 1.026 | 4.167 | 4.130 | 8.156 | 8.281 | 0 | measured |
| `diagnostic_hmm_example_no_island` | 22.222 | 22.451 | 1.012 | 11.230 | 11.302 | 8.703 | 8.953 | 0 | measured |
| `diagnostic_hmm_example_no_native_adj` | 29.714 | 33.144 | 1.118 | 11.869 | 11.976 | 8.766 | 9.312 | 0 | >5% slower; confirm |
| `diagnostic_lotka_volterra_no_direct_rk` | 42.736 | 44.323 | 1.038 | 1.520 | 1.454 | 5.844 | 5.906 | 0 | measured |
| `diagnostic_soil_incubation_no_direct_rk` | 59.413 | 61.722 | 1.042 | 1.708 | 1.942 | 6.516 | 6.656 | 0 | measured |
