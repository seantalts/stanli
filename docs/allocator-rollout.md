# Native allocator and alignment rollout measurements

2026-09-13. Default promotion is **on hold**: a repeatable local large-vector,
eight-worker loss tripped the [predeclared gate](allocator-rollout-plan.md).
The subsequent Linux screens also find 23% (ARM) and 29–34% (x86_64)
four-worker throughput losses in small models, each negative in five/five rounds.
The implementation remains SYSTEM / alignment OFF by default, with independent
explicit controls. [PR #362](https://github.com/seantalts/stanli/pull/362) is a
draft; auto-merge is not enabled while this decision and the remaining gates
are outstanding.

The [regression investigation](allocator-regression-investigation.md) now
separates startup behavior from steady-state throughput. On identical Linux
x86 binaries, normal 8/four workers changes from 0.688x to 1.070x simply by
settling the host before native execution. Limiting OpenBLAS threads alone
does not fix it. Historical short-warm screens below remain intact, but they
cannot be treated as clean evidence of steady-state allocator overhead.
The Apple allocator-only large/eight-worker loss persists with alignment ON
or OFF; a benchmark-only system fallback for large malloc requests recovers
only part of it and is not integrated. Defaults remain unchanged.

Sustained **native work**, rather than a pause, subsequently confirms the
measurement issue on both Linux architectures. The standard non-installed
evaluator now warms each worker for 500 ms; historical diagnostic drivers
request the old warmup explicitly. No warmup/sleep is added to shipping code.

| Focused warmed check / four workers | x86_64 ratio | ARM ratio |
| --- | ---: | ---: |
| Normal 8 | 1.011x | 1.085x |
| Eight Schools non-centered | 1.047x | 1.097x |
| Normal 1024 | 1.033x | 1.040x |
| Normal 262144 | 2.697x | 3.912x |

Each architecture completes 400 timed + 20 verification processes, with full
bytes and graph agreement. Normal 8 and Eight Schools losses reproduce in
the short-warm mode and recover in the sustained mode using the same DSOs;
normal 1024's historical short loss does not reproduce in this stage. Ranges
and controls are in the [investigation](allocator-regression-investigation.md)
and [evidence index](../tools/allocator/results/README.md). The x86 normal-8
result is near parity, not a substantial speedup. The first ARM attempt hit a
post-native diagnostics-collector race; its incomplete artifact is retained
separately from the one successful infrastructure retry.

Apple ARM remains unresolved after sustained work: large normal/eight workers
is 0.922x [0.915, 1.005], four/five negative, with noisier A/A controls. Large
normal/one worker also has a small negative in this focused stage. The accepted
change is to the evaluator, not to mimalloc/TLS/threshold policy. The original
tables below are historical short-warm measurements, not rewritten evidence.

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
release CI builds use the wheel's declared deployment targets. These runs on
current hosted runners do not constitute execution tests on the oldest
supported macOS versions.

## Native scope and compatibility

The [six-job native ownership matrix](https://github.com/seantalts/stanli/actions/runs/34749509047)
passed on macOS arm64/x86_64, Linux arm64/x86_64 (GCC and an additional Clang
check), and Windows MinGW/UCRT x86_64. It exercises C allocation versus C++
new/delete ownership, host/foreign pointers, reusable objects, dependency
archives, Eigen, exceptions, thread teardown, cross-thread frees, probe handle
close/reopen, and export isolation. Successful handle closure does not prove
that an OS unmapped the DSO; the shipping runtime and embedded compiler remain
loaded through process exit in these tests. The first Windows failure was a test-parser bug for
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

The subsequent [Windows shipping and timing job](https://github.com/seantalts/stanli/actions/runs/34750701616)
passes all 246 native tests, Python checks, and all 92 numerical verification
and 480 timing processes.
On this Windows Server 2022 x86_64 runner, the allocator-only ratios for
one/four workers are Eight Schools 1.440/1.420, hierarchical GP 1.120/1.132,
normal 8 1.626/1.660, normal 1024 1.081/1.055, and normal 262144 2.576/3.257.
Each of these cells improves in five/five rounds. Gamma 16384 is 0.998/1.000;
the small one-worker negative is retained, not rounded into a win. Both A/A
median ratios remain within 3% of one in every Windows cell, although the
large-normal controls have wider individual round ranges. All 12 cells,
ranges, memory measurements, and controls are in the
[Windows scorecard](../tools/allocator/results/ci-34750701616-windows-x86_64/summary.json).
The Linux metadata read failed before timing in that run; the corrected screen
subsequently completed on both Linux architectures in
[34751295812](https://github.com/seantalts/stanli/actions/runs/34751295812).

Linux ARM passes all 247 native tests, the full shipping job, 92 numerical
verification processes, and 480 timed processes. Its allocator-only
[scorecard](../tools/allocator/results/ci-34751295812-linux-arm64/summary.json)
shows a new important negative: normal 8/four workers is **0.772
[0.677, 0.778]**, losing five/five rounds, with SYSTEM/private A/A medians
1.015/0.980. This has not received a separate confirmation session, but cannot
be treated as a no-regression pass or dismissed by its much smaller controls.
The same normal 8 case improves 1.103x at one worker. Eight Schools is
1.089x/1.065x, hierarchical GP 1.052x/1.033x, normal 1024 1.034x/0.998x,
large normal 3.810x/3.973x, and gamma 16384 1.000x/0.993x (one/four workers).
The small four-worker gamma loss appears in four/five rounds with near-one
A/A medians. Large-vector wins do not cancel the tiny-vector parallel loss.

Linux x86_64 also completes all 92 verification and 480 timing processes.
Its allocator-only [scorecard](../tools/allocator/results/ci-34751295812-linux-x86_64/summary.json)
has three large four-worker losses, each negative in five/five rounds:

| Linux x86_64 / four workers | Candidate ratio [round range] | SYSTEM / private A/A medians |
| --- | --- | --- |
| Eight Schools non-centered | **0.661 [0.627, 0.682]** | 0.999 / 0.986 |
| Normal 8 | **0.707 [0.677, 0.734]** | 1.003 / 0.975 |
| Normal 1024 | **0.690 [0.669, 0.730]** | 1.000 / 0.990 |

Eight Schools also loses slightly at one worker: 0.986 [0.980, 0.986],
five/five negative rounds. Normal 8/one worker is 0.985 with four/five negative
rounds and controls 0.991/1.008. Hierarchical GP improves 1.024x/1.036x,
normal 1024/one worker 1.018x, and large normal 2.896x/2.838x (one/four workers).
Gamma is 1.001x/1.002x. These are first five-round CI screens, not additional
confirmation sessions. The large small-model parallel deficits are much larger
than their A/A shifts. No loop-alignment flag is applied on either Linux
architecture: those regressions already occur in the allocator-only candidate.

The final formatted local source passes **247 candidate / 243 SYSTEM native
CTest tests**, and each configuration passes **48 Python checks / 173 R
assertions**, plus BridgeStan embedding. Sixteen GC/model lifetimes, 4,096
changing host gradients, retained host arrays, and serial/parallel four-chain
sampling produce byte-identical complete output across both configurations in
each binding. A post-format recheck of all 52 native cells also matches the
screen's exact snapshots and graph dimensions; its calibration timings were
not used as performance evidence because test suites were running concurrently.

The [second Apple ARM host's measurements](https://github.com/seantalts/stanli/actions/runs/34749792182)
completed all 46 one/four-worker numerical cells and 480 timing processes on
macOS 15.7.9. In its six-model screen, Eight Schools improves 1.263x/1.261x
(one/four workers), hierarchical GP 1.049x/1.065x, normal 8 1.249x/1.246x,
normal 1024 1.039x/1.030x, and normal 262144 1.709x/1.965x. Gamma 16384 is
0.999x at both counts. The large-normal four-worker A/A medians also shift
about 9%, so the exact 1.965x should not be oversold. This screen did not test
eight workers and does not invalidate the local eight-worker regression.
The [Intel Mac screen](../tools/allocator/results/ci-34749792182-darwin-x86_64/summary.json)
also completes 92 verification and 480 timing processes. Eight Schools improves
1.250x/1.243x and hierarchical GP 1.167x/1.200x (one/four workers), each positive
in five/five rounds. Several other cells have large A/A shifts and broad ranges;
all of those controls remain in the results. Intel receives no loop-alignment
flag, so this comparison is allocator-only. The
[Apple ARM raw scorecard](../tools/allocator/results/ci-34749792182-darwin-arm64/summary.json)
measures the combined candidate. Neither screen substitutes for Linux/Windows
measurements.

A later [Apple ARM validation rerun](../tools/allocator/results/ci-34750701616-darwin-arm64/summary.json)
at formatted source is also retained in full, not substituted for the first
screen. It has substantial hosted-runner noise: normal 1024/four workers has
SYSTEM/private A/A medians 0.830/0.873, and gamma 16384/four workers shows a
0.908 candidate ratio alongside a 1.107 private A/A median. This is an
unresolved negative signal, not evidence of a clean no-regression pass.

The existing protected `manylinux_2_28_x86_64` PR gate now also requires the
native ownership matrix. Auto-merge must not bypass missing performance
evidence or a known regression merely because that CI gate is green.

If diagnosis continues, the next discriminating experiment is a matched
two-by-two Apple ARM ablation (SYSTEM/private allocator x loops OFF/ON),
focused on the large-normal/eight-worker loss and a positive control. The
combined comparison cannot attribute that loss to either change individually.
The new Linux deficits need a separate multithreaded allocator profile: they
occur without the loop flag and cannot be explained away as that Apple-only
alignment change. No Linux allocator-contention or TLS-cost cause is established
by these timing results alone.
No such additional timing experiment has been run, and no new allocator tuning,
CPU/model-specific policy, or regression allowance is inferred from these wins.

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
The fresh-process screen shows a counterexample: normal 262144/one worker's
median peak RSS rises from 64,782,336 B to 69,902,336 B. At eight workers it
falls from 153,534,464 B to 143,949,824 B. Retention, peak memory, code size,
and throughput therefore need separate judgments.

## Retained evidence and reproduction

[Local evidence](../tools/allocator/results/local-2026-09-13/README.md) includes
every cell/round and both A/A controls, exact commands and binary/source hashes,
all unique full-gradient snapshots, native logs, retention observations, and
host/native suite logs. Timed binaries are identified by their immutable
recorded hashes. The original post-format verification and subsequent bounded
regression-attribution experiments are recorded separately. They do not
replace the original corpus/confirmation observations.
The [evidence index](../tools/allocator/results/README.md) distinguishes the
first valid native CI screens, completed validation reruns, and unfinished runs.

Use the standalone commands in the evidence README to reproduce the matched
boundary. The existing CLI-versus-CmdStan table is historical and has not been
scaled by any allocator ratio. No new source-to-CSV or inference speedup is
claimed from a native warm-gradient measurement.
