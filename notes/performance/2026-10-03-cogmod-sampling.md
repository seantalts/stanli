# Cogmod families under sampling: OP_REGION_MAP with lanes

Per-gradient cost during NUTS sampling, all 22 cogmod families in
`tests/cogmod/` (practice copies with N = 2000, lnr_bench N = 4620).

Method: `stanli_run --seed 1 --warmup 50 --samples 50 --chains 1
--sampler-stats --save-warmup`; CmdStan `sample save_warmup=1 num_warmup=50
num_samples=50 random seed=1`. Per-gradient cost is wall time over the total
leapfrog steps of warmup and draws. With the same seed stanli takes exactly
CmdStan's leapfrog count on every family below except lnr, so the columns
follow the same trajectory. Five-minute limit per run. Apple M-series,
Release builds. v0.18.1 is the last release (82c605cd); the branch is
`feat/region-map`.

| family | branch | v0.18.1 | CmdStan |
| --- | ---: | ---: | ---: |
| gamma | 146 us | 275 us | 197 us |
| weibull | 141 us | 269 us | 193 us |
| logstudent | 133 us | 258 us | 186 us |
| invgamma | 322 us | 632 us | 344 us |
| invweibull | 329 us | 1086 us | 358 us |
| logweibull | 314 us | 1146 us | 380 us |
| bisa | 262 us | 1218 us | 439 us |
| loggamma | 352 us | 1864 us | 454 us |
| exgaussian | 285 us | 838 us | 412 us |
| geg | 510 us | 1469 us | 639 us |
| lognormal | 637 us | 17550 us | 755 us |
| exwald | 929 us | 26629 us | 1586 us |
| choco | 743 us | 2946 us | 812 us |
| betagate | 675 us | 1394 us | 597 us |
| lba1 | 776 us | over 5 min | 906 us |
| lba2 | 1381 us | over 5 min | 1423 us |
| rdm | 2294 us | over 5 min | 2425 us |
| lnr_bench | 1937 us | does not compile | 1265 us |
| lnr | 2636 us | does not compile | 2269 us |
| betadiscrete | 25558 us | 25658 us | 25312 us |
| ddm | over 5 min | does not compile | over 5 min |
| invgaussian | over 5 min | does not compile | over 5 min |

v0.18.1 does not compile lnr and lnr_bench (`log_mix` inside a parameter
branch) nor ddm (six-argument `wiener_lpdf`); invgaussian fails on a guard
return. lnr's trajectory diverges from CmdStan's (5812 vs 7124 leapfrogs).
betadiscrete has no parameter-dependent control and keeps its old lowering.
ddm and invgaussian run numerical integration per observation and exceed the
limit in CmdStan too.

Engine per family (`STANLI_REGION_MAP_DIAGNOSTICS=1`): lanes for 20 families,
scalar recompute for invgaussian (its body has `LSE_RANGE`, and one 64-lane
tile would be about 48 MB), no map for betadiscrete.

How the branch got here, gamma per gradient on the same trajectory, draws-only
counting (the earlier convention): structured loop 485 us, first scalar map
1112, saved state instead of recomputing plus hoisted constants 1137 (memmove
per span ate the gain), merged spans 868, conditions as jumps and flags only
for segment blocks 633. Warmup-counted from there: lanes 244, seed list and
one adjoint zero per backward 146.

## Other corpus models

A census of all 351 corpus models (`STANLI_REGION_MAP_DIAGNOSTICS=1`) finds
the map in 24: the 21 mapped cogmod families and three brms models whose
likelihood UDF branches on a parameter. All 24 run lanes except invgaussian.
No corpus model selects the structured loop any more. Four brms models
(sw_hurdle_lognormal, sw_hurdle_gamma, s2_zi_beta, s2_zoi_beta) branch on
data inside the UDF; an earlier selection test counted those as parameter
branches and made them 1.4x to 2.9x slower than v0.18.1. Selection now
follows data through UDF arguments, and they keep their old lowering.

Sampling, 10000 warmup and 10000 draws, same trajectory (N = 40, so per
gradient is small):

| model | branch | v0.18.1 |
| --- | ---: | ---: |
| s2_gev | 3.7 us | 7.9 us |
| sw_asymlaplace | 3.2 us | 5.4 us |
| s2_zi_asymlaplace | 4.9 us | 5.5 us |
| sw_hurdle_lognormal (not mapped) | 2.2 us | 2.3 us |
| sw_hurdle_gamma (not mapped) | 3.5 us | 3.6 us |
| s2_zi_beta (not mapped) | 6.2 us | 6.0 us |
| s2_zoi_beta (not mapped) | 6.3 us | 6.3 us |

Memory: lanes keep every tile's forward state. lnr_bench peaks at 138 MB
resident with lanes, 84 MB without the map, 50 MB with the scalar map
recomputing. gamma peaks at 20 MB.
