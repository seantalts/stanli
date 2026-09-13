# Native allocator and alignment rollout measurements

2026-09-13. Default promotion is **on hold**: a repeatable large-vector,
eight-worker loss tripped the [predeclared gate](allocator-rollout-plan.md).
The implementation remains SYSTEM / alignment OFF by default, with independent
explicit controls. [PR #362](https://github.com/seantalts/stanli/pull/362) is a
draft; auto-merge is not enabled while this decision and the remaining gates
are outstanding.

## Matched local screen and confirmation

Fresh current-main baseline `7bbda72c008c16cf227c4db84b0d7f95a3ffc65e`, using
the implementation at `cf03f1c1`, Release Apple Clang 21, M3 Ultra, arm64,
`-O3 -ffp-contract=off`. Compare SYSTEM/unmodified loop layout with private
mimalloc 3.5.1/32-byte loop alignment. This measures native warm sampler
gradients inside a non-installed DSO loaded after Python/NumPy, not Python
call overhead, source compilation, sampling completion, or CmdStan.

All 23 frozen models, one/four workers, and six selected eight-worker cells:
52 cells. Five balanced rounds, two processes per slot/round, three blocks
per process, baseline-only 150 ms calibration, and identical-binary aliases
for BOTH variants: 2,080 timing processes. All full log-density/gradient
snapshots and graph dimensions agree bitwise at eight changing points,
before and after replay. The 104 verification processes also passed. The
portable evaluator matched the retained evaluator on all 23 models.

Ratios below are paired baseline/candidate time: >1 means more throughput.
Brackets give the full range of round ratios, not confidence intervals.
Multi-worker values measure aggregate throughput, not one chain's latency.
Absolute medians and median paired ratios are different estimators.

| Model / workers | Screen ratio [round range] | Confirmation ratio [round range] |
| --- | --- | --- |
| Eight Schools non-centered / 1 | 1.247 [1.238, 1.299] | 1.241 [1.158, 1.324] |
| Hierarchical GP / 4 | 1.120 [1.113, 1.125] | 1.118 [1.113, 1.127] |
| Normal 1024 / 4 | 1.308 [1.297, 1.325] | 1.315 [1.293, 1.349] |
| Normal 262144 / 8 | **0.942 [0.888, 0.996]** | **0.948 [0.876, 0.985]** |
| Diamonds / 4 | 0.957 [0.945, 1.065] | 1.007 [0.954, 1.091] |
| Soil incubation / 4 | 0.953 [0.868, 1.044] | 0.971 [0.836, 1.039] |

The fixed confirmation selected every screen cell below 0.97 plus three
predeclared controls: seven rounds, unchanged binaries/calibration, 336 timing
processes, all complete-byte checks passed. Normal 262144/eight workers loses
in **12/12** rounds across the two sessions. Confirmation SYSTEM/private A/A
medians are 0.983/0.997: controls are not perfectly stationary, but this is not
a defensible claim of no regression. The three positive controls win 12/12.
Diamonds' negative result does not repeat. Soil is still unresolved: five of
seven confirmation rounds lose, but private A/A shifts 1.054 and ranges widely.

Other screen wins include kidscore/four workers (1.464), GP regression/four
workers (1.343), and normal 128/four workers (1.386). Do not promote the noisy
Poisson/four-worker headline (1.461) as a clean allocator effect: its SYSTEM
A/A median shifts 1.151. Every cell and both controls must remain visible in
the retained results; no aggregate speedup cancels a losing workload.

## Linked code and size

The native runtime compile commands differ only by `-falign-loops=32`; all
53 SUNDIALS objects are byte-identical. The embedded compiler's full provenance
stamp matches the current source inputs and pinned stanc3 revision.

The formerly sensitive normal NaN-check loop retains exactly the same eight
instructions in shipping, benchmark, and CLI binaries. All aligned versions
start on 32-byte boundaries and do not cross 4 KiB. The current SYSTEM loops
also do not cross 4 KiB. Thus the new eight-worker slowdown is not evidence
that the old specific boundary-crossing problem has returned; its cause is
not established by these measurements.

| Local shipping DSO | File bytes | Executable `__text` bytes |
| --- | ---: | ---: |
| SYSTEM / OFF | 39,039,520 | 25,905,544 |
| Private mimalloc / aligned | 40,846,512 | 27,665,156 |

This is the combined allocator/alignment size change, not an alignment-only
attribution. The local embedded object carries a newer deployment target;
minimum-OS compatibility is checked by the actual CI shipping jobs instead.

## Native scope and compatibility

The [six-job native ownership matrix](https://github.com/seantalts/stanli/actions/runs/34749509047)
passed on macOS arm64/x86_64, Linux arm64/x86_64 (GCC and an additional Clang
check), and Windows MinGW/UCRT x86_64. It exercises C allocation versus C++
new/delete ownership, host/foreign pointers, reusable objects, dependency
archives, Eigen, exceptions, thread teardown, cross-thread frees, unload/reload,
and export isolation. The first Windows failure was a test-parser bug for
GNU objdump's extra ordinal column; the corrected parser and format fixtures
pass. No allocator export was waived or added to the allowlist.

The first full shipping run passed both Mac jobs and both Linux jobs. Windows
passed 243/245 native tests; its two export-parser failures use the now-fixed
checker. The corrected Windows shipping library subsequently passed its full
native and Python suites; the optional benchmark target then exposed a missing
mimalloc include path in its copied ELF/PE input archive (also on Linux ARM).
That measurement-target wiring is corrected separately from the allocator.
Native CI performance results are not claimed complete until their artifacts
have been reviewed.

The final formatted local source passes **247 candidate / 243 SYSTEM native
CTest tests**, and each configuration passes **48 Python checks / 173 R
assertions**, plus BridgeStan embedding. Sixteen GC/model lifetimes, 4,096
changing host gradients, retained host arrays, and serial/parallel four-chain
sampling produce byte-identical complete output across both configurations in
each binding. A post-format recheck of all 52 native cells also matches the
screen's exact snapshots and graph dimensions; its calibration timings were
not used as performance evidence because test suites were running concurrently.

The existing protected `manylinux_2_28_x86_64` PR gate now also requires the
native ownership matrix. Auto-merge must not bypass missing performance
evidence or a known regression merely because that CI gate is green.

## CLI alignment and memory retention

The unmodified static `bench_grad` consumer uses SYSTEM in both builds. Its
fixed point and 200 ms warmup were retained, with baseline-only 150 ms gradient
calibration: 23 calibration processes and 460 timing processes, five balanced
rounds, both identical-binary aliases. This screen is **inconclusive**, not a
no-regression pass. For example, radon pooled's 0.941 ratio accompanies a 0.896
SYSTEM A/A median; GP's SYSTEM and aligned A/A medians both shift to 0.913.
The one-compartment ODE ratio is 0.968 [0.883, 1.010], losing four/five rounds
despite near-one A/A medians. These signals cannot justify a blanket CLI
alignment speedup or an unsupported claim that it never regresses.

The fixed long-lived NumPy-host retention run completed **1,179,648 gradients
per variant**, plus warmup and checks: 12 small/large/small normal sequences,
four workers, 1,024 repetitions x eight blocks. Every snapshot matches across
both variants and all lifetimes. No forced allocator collection or option
tuning. After each model is destroyed, RSS plateaus; it is unchanged across
the final five full sequences:

| Configuration | Final RSS | Process peak RSS |
| --- | ---: | ---: |
| SYSTEM / OFF | 131,497,984 B | 131,629,056 B |
| Private / aligned | 96,043,008 B | 96,174,080 B |

That is lower retained memory in this stress case, not a general maximum-RSS
guarantee or proof that every application lifetime is leak-free. Raw block,
join, and destruction observations and the full snapshots are retained.

## Retained evidence and reproduction

[Local evidence](../tools/allocator/results/local-2026-09-13/README.md) includes
every cell/round and both A/A controls, exact commands and binary/source hashes,
all unique full-gradient snapshots, native logs, retention observations, and
host/native suite logs. Timed binaries are identified by their immutable
recorded hashes; later changes are formatting, fallback/configuration tests,
documentation, and the ELF/PE measurement-target include path, not additional
timing runs. The post-format verification is recorded separately.

Use the standalone commands in the evidence README to reproduce the matched
boundary. The existing CLI-versus-CmdStan table is historical and has not been
scaled by any allocator ratio. No new source-to-CSV or inference speedup is
claimed from a native warm-gradient measurement.
