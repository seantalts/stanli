# ctsem recording-site results

This comparison isolates the index and iterator specializations added after
the temporary compiled recorder. It does not combine timings from earlier
PR391/frame experiments. The base remains PR391's merge,
`0daf3b15452d758f290afffe70a1cf8e4759310b`.

Six alternating fresh-process pairs use identical benchmark-driver objects,
single-thread settings, 300 ms warmup and 1000 ms measurement, with separate
fresh preparation runs. The [machine-readable results](2026-09-20-ctsem-recording-sites.json)
preserve exact statistics, source/binary identities, all 319 exploratory
ordinary-case summaries, focused controls and the final profile. Raw logs
and scripts remain in `.cache/ctsem-index/`.

| ctsem rows | First gradient, preceding recorder | First gradient, new recorder | Reduction |
| --- | --- | --- | --- |
| 400 | 1.963 s | 1.569 s | 20.1% |
| 4000 | 19.367 s | 15.273 s | 21.1% |

At 4000 rows, warmed gradients are 497.36 / 497.70 ms and preparation is
1.3848 / 1.3908 seconds. Timing and peak-RSS intervals include parity for
these phases at all four target sizes. Live bytes and allocation counts
match exactly at every paired target boundary. The new paths activate only
after frame admission, preserve dynamic checks and original fallbacks, and
add no ordinary preparation pass or retained recorder cache.

Default and forced-frame reference checks each pass 329 models at three
points, comparing 1,020,194 values with worst scaled error 9.38e-13. Ctsem
matches PR391 bitwise at four sizes and three points, 581 values per point.
Targeted ASan/UBSan passes. CTest passes 260/262 with the two unchanged
signature-manifest freshness failures.

## Ordinary controls and limitations

All 316 finite cases in the 319-case exploratory sweep match bitwise; the
same three benchmark points are nonfinite in both binaries. Median warmed
and preparation ratios are 0.99721 and 1.00446. Those short-run aggregate
statistics do not establish the absence of every per-model regression.
First-gradient and warmed live bytes/counts match in every finite case.

Seven independent six-pair controls produced two signals. Fixed six-A/B and
six-identical-binary-A/A follow-ups give these results:

- The mixed-cell canary's warmed timing includes parity: A/B ratio interval
  0.96332–1.01815, A/B minus A/A log-ratio interval 0.94358–1.02105.
- `s2_gev` retains a peak-RSS signal: medians 6,733,824 / 6,848,512 bytes,
  a 114,688-byte (112 KiB) increase. The A/B ratio interval is
  1.01131–1.02029 and the A/A-adjusted interval is 1.00763–1.02318.
  Live allocation bytes/counts are exactly equal. This is unresolved;
  binary layout or allocator behavior is not a demonstrated cause.

One exploratory `ch12_m12_5` preparation process reported 29,684,320 live
allocator bytes instead of 32,813,664, always with 50,969 blocks. A separately
fixed 72-process preparation control reproduces both readings in each
revision and in identical-binary A/A labels. A/B has the same distribution
in both revisions: 3 lower and 15 higher readings. This is no evidence of
a candidate memory improvement. All post-gradient readings agree.

The earlier [ordinary timing/RSS investigation](2026-09-20-ctsem-ordinary-model-controls.md)
remains deferred at the user's request. This step establishes the ctsem
recording benefit and numerical behavior, but does not establish universal
ordinary-model process-RSS parity. No padding, linker-order tuning, or
additional adaptive timing rounds are included.

## Remaining work

The final ten-second profile contains 7,698 main-thread samples, all during
first recording. Recording instructions contain 44.5% exclusively; this
includes the new inlined index checks and iterator updates, so it is not
all removable dispatch overhead. Frame sealing accounts for 24.7% including
callees, and the general index validator for 4.9%. Iterator binding accounts
for only 32 samples including callees, down from 669 in the index-only
profile. Shares describe sampling intervals, not additive speedup ceilings.

The next experiment should distinguish repeated operand lookup/dispatch
from frame encoding before selecting another change. Direct emission into
a proven frame layout could avoid rebuilding encoding metadata, but needs
a separate lifetime and variable-shape proof; it remains unimplemented.
The [implementation/proof log](../../docs/superpowers/plans/2026-09-20-ctsem-index-recording.md)
contains the eligibility rules, adversarial tests, ablations and experiment
sequence.
