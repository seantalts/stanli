# ctsem data-loop memo results

First recording of ctsem's `ctsm.stan` re-ran the same table scans for every
subject. `mcalc` and `parvectform` walk all 1,440 rows of `matsetup` once per
call and keep a handful of rows. At 4,000 rows the recording program executed
about 249M instructions per 200 subjects, and most of them decided which rows
to keep. Those decisions depend only on model data, literal arguments, and the
subject index through `si == 0`.

The memo traces a data-controlled loop the first time it runs with a given
key and replays the trace on later runs. A hit executes only the instructions
that touch parameters, in their original order, through the normal recorder.
The memo exists during one first recording and is freed with it.

## Results

Apple M3 Ultra, Apple clang 21, arm64 Release, pinned dependencies, saved O0
MIR. Three fresh-process pairs per size, alternating order, one thread. The
host carried a load average of 2.9 to 3.9 from other sessions. Medians:

| Rows | First gradient, off → on (s) | Warm gradient, off → on (ms) | Preparation, off → on (s) | Peak RSS, off → on (MB) |
| ---: | ---: | ---: | ---: | ---: |
| 400 | 1.317 → 0.614 | 38.99 → 39.03 | 1.468 → 1.480 | 273.4 → 275.0 |
| 1,200 | 3.812 → 1.684 | 118.81 → 118.68 | 1.129 → 1.115 | 522.6 → 524.3 |
| 4,000 | 12.542 → 5.432 | 399.35 → 398.80 | 1.177 → 1.185 | 1,490.2 → 1,491.8 |

The first gradient is 2.1x, 2.3x and 2.3x faster. The off column is
`STANLI_NO_DATA_LOOP_MEMO=1` on the same binary. With the memo off, that binary
matches the pre-change binary at 400 rows (1.29 s against 1.33 s). Peak RSS
rises by 1.6 to 1.7 MB at every size, 0.1% at 4,000 rows. The memo tables take
373 KB at 4,000 rows, so most of the increase is elsewhere; the recorded
frames use 6 distinct programs instead of 4 and 753K code words instead of
639K, because replayed iterations place saved operands in a different order.

Log density and all 580 gradients are bitwise identical to memo off at 400,
1,200 and 4,000 rows at five points, and for both `tests/ctsem` fixtures with
frames forced. Most of the remaining 4,000-row recording time is frame sealing
and the parameter-dependent kernels.

## What a hit skips

An instruction inside the loop is skipped when its only effect is a data
value: data kernels, kernels whose inputs are all constant and which log
nothing, aliases of constant values, and the branches and loop counters they
control. Everything else is a step and runs again: logged kernels, in-place
updates, targets, and aliases of non-constant values. The scan in `mcalc`
builds each row's condition from folded kernels and aliases, so skipping only
the instructions already classified as data (`DataKernel`, `DataBranch` and
friends) would still replay tens of millions of kernel and alias executions at 200
subjects.

Before a step, the trace stores the values of the data it reads: workspace
cells written by skipped instructions, and slots bound to skipped values. A
hit writes those back before the step. After the last step it restores the
final values of data slots that are read outside the loop. A skipped value
that must exist as a version is re-created as a fresh constant version holding
the recorded values.

## Proof obligations

- The key holds every value a skipped instruction can read before the loop
  has written it, plus whether each such value is constant. A static
  definite-assignment pass over the loop's jumps finds those reads. Slots that
  no instruction in the body ever writes, including imports and literals, are
  immutable for a recording and stay out of the key.
- A loop-invariant data kernel, whose inputs are all unwritten in the loop,
  is evaluated on private buffers and its output is keyed instead of its
  inputs, but only when every other skipped reader of those inputs is also
  such a kernel. A raw read of the same value keeps the raw value in the key.
  A kernel that throws at key time leaves the key unavailable, so the loop
  runs normally and throws where it did before.
- A branch, loop bound or skipped instruction that reads a slot last written
  by a step makes the trace unusable. This was the one defect the randomized
  differential test found: a cached invariant kernel whose version predates
  the trace was read by a skipped alias.
- A branch or loop bound that is not constant would log a guard. Such a loop
  is never memoized.
- Steps run in trace order, so the order and operands of effectful steps are
  unchanged. Generation counters of loops entered inside the loop are bumped
  at the same points relative to the steps, so invariant-kernel caches behave
  as in the original run.
- A trace that throws stores nothing. A key that was traced and rejected is
  remembered, so the loop is not traced again.
- Limits keep the cost bounded: 256 memos per loop, 64K events and 1M
  doubles per memo, a loop that executes fewer than 256 instructions per
  trace is dropped, a trace is dropped when replay would not execute at least
  8 times fewer instructions, and a loop that hits less than twice per refused
  entry is retired after 32 attempts. `STANLI_DATA_LOOP_MEMO_MIN_WORK` and
  `STANLI_DATA_LOOP_MEMO_MIN_RATIO` set the first two.

## Validation

`tests/test_structured_loop.cpp` compares memo on and off bitwise for fixtures
that vary control with the outer iteration, write a local in the loop and read
it after, break and continue, nest data loops, fail an index on the first and
on a later outer iteration, read a non-import outer value, read an outer value
both raw and through an invariant comparison, write matrix cells addressed by
table columns, keep effectful steps in order, and recover from a failed
recording. A randomized generator builds 400 plans per run from data kernels,
folded kernels, aliases, branches, for and while loops, break and continue,
in-place data and parameter updates, and targets, and also changes the data
after the first evaluations. 100,000 plans pass bitwise. Five of the first
20,000 fail when the stepped-slot check is removed; those seeds stay in the
default run. The full suite and 4,000 plans pass under AddressSanitizer and
UndefinedBehaviorSanitizer.

CTest passes 426 of 426 and the recorded CmdStan corpus replay passes 357 of
357 models (1,043,903 values, worst scaled error 9.38e-13). A diagnostic sweep
of the same 357 models shows that none records frames by default, the two
`ctsem` fixtures included, so the replay does not exercise the memo and no
corpus model runs the changed code. With frames forced, both fixtures match
memo off bitwise.

## Limits

Validation is local arm64 only. The memo applies only after frame admission,
so models that do not record frames never build the analysis. The first trip
that meets each key pays a traced run; the 4,000-row run has 32 traced entries
and 19,953 hits. The full-corpus benchmark of
[docs/benchmarks.md](../../docs/benchmarks.md#reproduction) was not run; it
builds CmdStan for each of the 343 models.
