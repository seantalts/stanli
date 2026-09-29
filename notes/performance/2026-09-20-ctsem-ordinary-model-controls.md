# Deferred research: ordinary-model timing and RSS controls

On 2026-09-20 the user requested that these residuals be ignored for now and
preserved in a notes directory. They do not block the next ctsem optimization.
The observations remain unresolved; deferral is not a finding of zero cost.

The comparison is the completed frame implementation versus the subsequent
temporary recording program with data-only instructions. Both derive from
PR391's merge, `0daf3b15452d758f290afffe70a1cf8e4759310b`.
See the [implementation draft](../../docs/superpowers/plans/2026-09-20-ctsem-recording-draft.md)
and [main experiment log](../../docs/superpowers/plans/2026-09-19-ctsem-memory.md).

The [machine-readable summary](2026-09-20-ctsem-ordinary-model-controls.json)
preserves exact statistics, control results and source/binary hashes in the
repository. Full per-process samples and command logs remain local ignored
artifacts under `.cache/ctsem-first/`; they are not included in that summary.

## Protocol, findings and attribution limits

The two-pair ordinary sweep covers 319 cases. All 316 finite cases match
bitwise and have exactly equal live bytes and allocation counts at all three
phase boundaries. `dogs_log`, `s2_invgaussian` and `sir` are nonfinite in both
binaries at the benchmark point, as in the earlier sweep. Seven independent
12-pair phase controls include the only ordinary retained-loop case,
`s2_gev`, and the mixed-cell canary. No timing interval for `s2_gev` excludes
parity. The short sweep intentionally triggers a broad fixed follow-up set:
192 cases have a timing/RSS signal not already resolved by those controls.
Each receives six A/B and six identical-binary A/A pairs. First-only and
RSS-only checks use short evaluation windows; warmed-gradient flags use
300/700 ms windows, and preparation uses the median of three fresh
processes per label. Thresholds select investigations, not an acceptable
regression allowance. All initial and confirmation samples are retained.

An early ordinary signal prompted a build-identity audit: all other 67
archive members are byte-identical; only `structured_loop.cpp.o` and the
archive symbol index differ. The benchmark-driver object is identical.
Function addresses in otherwise unchanged objects do move after relinking,
including the island adjoint interpreter. Fable inspected this attribution
question with its own tools in a third explicit Fable 5.1 invocation; its
raw response is `fable-validation-review.jsonl`. It found no added executed
path for models without retained loops. Its stronger claim that layout is
already a demonstrated cause goes beyond the evidence: changed addresses
are a hypothesis until isolated. Its 85 KB page-count example also assumes
4 KiB pages rather than this host's 16 KiB pages. No padding or platform
function-order tuning is applied to the production code.

The diagnostic pause of the confirmation orchestrator exceeded its subprocess
timeout even though that child benchmark had finished. The entire incomplete
`aalto_binomb` confirmation was preserved under `confirmation-interrupted/`
and restarted from its first pair. Sixteen complete case records preceding
the pause were retained. No result from the interrupted case is reused and
no runtime source changed.

### Completed ordinary controls and open acceptance gate

All 192 fixed confirmation cases completed. The 224 preselected metric
checks use the original six A/B and six A/A pairs, with no adaptive extension.
For each metric, the additional comparison subtracts the paired A/A log
ratio from the A/B log ratio. These are pointwise descriptive 95% intervals,
not a simultaneous confidence claim across the whole family.

| Metric | Selected checks | A/B lower bound above parity | Both A/B and A/B-versus-A/A lower bounds above parity |
| --- | --- | --- | --- |
| Warm gradient | 83 | 9 | 4 |
| First gradient | 31 | 20 | 15 |
| Preparation | 82 | 3 | 0 |
| Process peak RSS | 28 | 15 | 8 |

Every checked live allocation byte count and block count is identical at
preparation, first-gradient and warmed boundaries. Allocator reserved bytes
can differ, including between identical-binary A/A runs. In the largest
remaining RSS example, `gpcm_latent_reg_irt`, median peak RSS is 61.55 MB
versus 63.61 MB. Live storage is exactly 7,143,664 bytes in both; allocator
reserved storage is 49,283,072 versus 51,380,224 bytes. A gap is already
present in prep-only runs. The extra reserved pages cannot be described as
an increase in live model storage, but neither can they be ignored when
assessing the user's process-memory requirement.

The four warmed residuals are `Rate_1_model` (75.09 / 77.23 ns),
`ch09_m9_5` (238.46 / 241.92 ns), `s2_car_esicar` (908.63 / 931.78 ns),
and `s2_gr_student` (1221.93 / 1276.69 ns). First-call examples include
`sw_hurdle_gamma` (16.92 / 19.81 microseconds) and
`sw_hurdle_lognormal` (14.06 / 17.27 microseconds). The first-call pattern
is too broad to dismiss merely by invoking multiple comparisons. No new
recorder is constructed on those paths. The current evidence establishes
the absence of added recorder work and live allocations there, not absence
of every elapsed-time or process-RSS cost.

The independent 12-pair N32 RSS confirmation includes parity: A/B ratio
interval 0.99766–1.00660 and A/B-versus-A/A 0.99728–1.00703. All 48 processes
have exactly 59,528,432 live bytes and 85,650 blocks after the first gradient.

A fixed second experiment crosses source revision with the Darwin linker's
documented `-no_deduplicate` setting, with twelve counterbalanced pairs per
setting. It changes no production flags or source. For `Rate_1_model`, even
the original default-link binaries now include parity (75.96 / 75.74 ns,
ratio interval 0.98100–1.01217). The no-deduplication interval also includes
parity, and the interaction interval 0.94505–1.01223 does not isolate a
deduplication effect. `2pl_latent_reg_irt` still has an RSS signal under both
link modes; its interaction interval 0.99107–1.01425 is inconclusive.
This does not erase the earlier observations or prove their cause. No
binary padding, function-order changes, or production linker tuning is
justified by this experiment.

These results did not establish the original strict ordinary-model
performance/process-memory requirement. The user subsequently deferred
this investigation; it no longer blocks the current ctsem work. If it is
resumed, isolate a concrete cause before changing the runtime. Repeating
the same timings until every interval includes parity would not resolve it.

## Complete remaining case list

The tables include the 27 selected metric checks whose original A/B and
A/B-versus-A/A 95% lower bounds both exceeded parity. Before/after values
are raw medians. The final column is the paired A/B-versus-A/A log-ratio
interval, expressed as percent change; it is not an interval around the
ratio of medians. These are per-case intervals, without a family-wide
correction. The later Rate_1_model control included parity, as recorded above.

### Warmed gradient

| Model | Before (ns) | After (ns) | Median change | A/B-versus-A/A 95% change |
| --- | ---: | ---: | ---: | --- |
| `Rate_1_model` | 75.093 | 77.225 | +2.84% | +0.54% to +4.63% |
| `ch09_m9_5` | 238.456 | 241.916 | +1.45% | +0.26% to +3.37% |
| `s2_car_esicar` | 908.626 | 931.779 | +2.55% | +1.10% to +4.75% |
| `s2_gr_student` | 1221.933 | 1276.691 | +4.48% | +0.36% to +6.80% |

### First gradient

| Model | Before (microseconds) | After (microseconds) | Median change | A/B-versus-A/A 95% change |
| --- | ---: | ---: | ---: | --- |
| `ch13_m13_5` | 23.334 | 26.396 | +13.12% | +0.86% to +30.64% |
| `i319_pois_re2` | 27.375 | 30.250 | +10.50% | +6.50% to +26.03% |
| `multi_occupancy` | 75.271 | 79.645 | +5.81% | +1.65% to +21.09% |
| `pilots` | 10.208 | 11.146 | +9.18% | +3.60% to +42.42% |
| `s2_car` | 22.709 | 25.209 | +11.01% | +1.37% to +46.34% |
| `s2_cens_interval` | 15.646 | 17.562 | +12.25% | +1.78% to +26.59% |
| `s2_cox` | 13.624 | 16.625 | +22.02% | +2.71% to +45.96% |
| `s2_cox_cens` | 14.876 | 18.917 | +27.17% | +11.68% to +49.58% |
| `s2_me2` | 46.709 | 53.645 | +14.85% | +6.64% to +36.96% |
| `s2_rate` | 9.229 | 11.041 | +19.64% | +6.74% to +51.97% |
| `s2_unstr` | 52.251 | 62.605 | +19.82% | +4.70% to +34.22% |
| `sw_hurdle_gamma` | 16.917 | 19.812 | +17.11% | +25.14% to +37.29% |
| `sw_hurdle_lognormal` | 14.062 | 17.270 | +22.81% | +21.98% to +39.94% |
| `sw_mi` | 16.375 | 19.520 | +19.21% | +6.43% to +35.72% |
| `sw_weibull` | 18.375 | 21.125 | +14.96% | +10.72% to +25.94% |

### Peak process RSS

| Model | Before (MB) | After (MB) | Median change | A/B-versus-A/A 95% change |
| --- | ---: | ---: | ---: | --- |
| `2pl_latent_reg_irt` | 8.610 | 8.700 | +1.05% | +0.15% to +1.76% |
| `ch14_m14_2` | 10.215 | 10.273 | +0.56% | +0.14% to +1.47% |
| `gpcm_latent_reg_irt` | 61.546 | 63.611 | +3.35% | +1.85% to +3.94% |
| `hier_2pl` | 12.337 | 12.526 | +1.53% | +0.78% to +2.15% |
| `i320_sratio_plain` | 15.737 | 16.056 | +2.03% | +1.36% to +2.74% |
| `sw_arma` | 8.200 | 8.258 | +0.70% | +0.42% to +1.66% |
| `sw_cumulative_cs` | 13.328 | 13.558 | +1.72% | +0.85% to +3.87% |
| `sw_negbinomial` | 5.546 | 5.636 | +1.62% | +0.02% to +3.45% |

## Artifacts and possible follow-up

- `.cache/ctsem-first/confirmation/selection.json`: frozen selection and protocol.
- `.cache/ctsem-first/confirmation/*.result.json`: fixed A/B and A/A samples.
- `.cache/ctsem-first/small-rss-confirmation/`: twelve-pair N32 RSS confirmation.
- `.cache/ctsem-first/layout-control/`: crossed linker settings, symbols and hashes.
- `.cache/ctsem-first/archive-member-identities.json`: only the structured-loop object differs.
- `.cache/ctsem-first/integrated-identities.json`: validated source and binary identities.
- `.cache/ctsem-first/fable-final-attribution.jsonl`: final Fable 5.1 review with its own tools.

A baseline binary with unused text, together with first-call page-fault
measurements, could test the executable-layout hypothesis. Allocation-lifetime
and reservation traces could investigate the RSS gap. These are optional
future experiments, not scheduled work. Neither a reproduced layout cost
nor variable A/A reservations alone would prove that an A/B cost is absent.
