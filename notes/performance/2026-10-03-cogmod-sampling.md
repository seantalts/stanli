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
