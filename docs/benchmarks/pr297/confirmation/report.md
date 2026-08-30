# Runtime before/after benchmark

Before: `917c634170420071f76fb6fb90a5c47fd708c7ab`. After: `8d8b37f79dd9b5c5e408a33321664fd86a5d4f36`.

All ratios are after / before; lower is faster. Timings are medians; raw paired samples and quartiles are in results.json. Numerical ULPs cover LP and every gradient at all requested points; failures are not dropped.

| Model | Before grad (µs) | After grad (µs) | Ratio | Before prep (ms) | After prep (ms) | Before RSS (MiB) | After RSS (MiB) | Max ULP | Status |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `accel_gp` | 5.783 | 6.336 | 1.092 | 5.399 | 5.420 | 9.797 | 10.109 | 0 | >5% slower; confirm |
| `accel_splines` | 6.411 | 6.876 | 1.073 | 3.921 | 3.918 | 8.078 | 8.219 | 0 | >5% slower; confirm |
| `garch11` | 7.308 | 8.324 | 1.130 | 2.369 | 2.335 | 6.250 | 6.344 | 0 | >5% slower; confirm |
| `hmm_drive_0` | 107.532 | 128.435 | 1.185 | 53.726 | 56.384 | 19.297 | 21.250 | 0 | >5% slower; confirm |
| `hmm_drive_1` | 112.995 | 129.634 | 1.149 | 54.186 | 55.140 | 19.500 | 20.562 | 0 | >5% slower; confirm |
| `hmm_example` | 16.625 | 19.474 | 1.167 | 11.634 | 11.878 | 8.625 | 9.094 | 0 | >5% slower; confirm |
| `hmm_gaussian` | 171.313 | 195.230 | 1.143 | 256.289 | 258.005 | 60.953 | 66.578 | 0 | >5% slower; confirm |
| `iohmm_reg` | 164.094 | 188.845 | 1.151 | 357.540 | 362.397 | 71.031 | 70.391 | 0 | >5% slower; confirm |
| `kronecker_gp` | 194.141 | 186.518 | 0.962 | 6.377 | 6.321 | 8.656 | 8.703 | 0 | measured |
| `lotka_volterra` | 21.304 | 26.144 | 1.220 | 1.393 | 1.380 | 5.656 | 5.719 | 0 | >5% slower; confirm |
| `normal_mixture_k` | 181.193 | 188.149 | 1.037 | 28.811 | 28.839 | 34.031 | 33.797 | 0 | measured |
| `soil_incubation` | 28.141 | 36.231 | 1.286 | 1.679 | 1.688 | 6.391 | 6.453 | 0 | >5% slower; confirm |
