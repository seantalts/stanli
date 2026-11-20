# Data-loop memo and frame instances: which models reach them

Date: 2026-11-20. Base: `fix/audit-nov20` at bcc36cc62, branch
`fix/audit5-loops`. Evidence for a decision on whether the two mechanisms
stay. Nothing here is a timing claim: every number below is a deterministic
count, and the timing that would decide is prepared but not run.

## What the two mechanisms are

Both run only while a retained loop records frames, which is how a long
`OP_LOOP` builds its tape on the first gradient.

- **Data-loop memo** (`runtime/src/structured_memo.inc` and
  `structured_memo_run.inc`, 805 lines). Inside a recording, a loop whose
  control depends only on data is traced the first time it runs with a given
  key. Later runs with the same key replay the trace: they redo only the steps
  that touch parameters and skip the data-only instructions. The memo is built
  when the recording program is compiled and is freed with the recording.
  [The 2026-10-25 note](2026-10-25-ctsem-data-loop-memo.md) has the design and
  proof obligations.
- **Frame instances** (`runtime/src/structured_instance.inc` and
  `structured_instance_run.inc`, 1,917 lines; the second file also holds a
  few small helpers that the recording program uses). While one trip of the outer loop records, it also
  records a data plan. A later trip whose recorder state matches a template
  copies the template frame, patches its data and relocates its bindings
  instead of recording the trip again. A mismatch records the trip normally.

Without them, the recording program runs every instruction of every trip.
Warm gradients replay the sealed frames and never reach either mechanism.

## When frames are recorded

`prefer_frames` in `runtime/src/structured_loop.cpp` switches a recording to
frames after 8 trips when the version table that the whole loop would need
is predicted to exceed 128 MB, and the loop runs at least 128 recording
instructions per trip. `STANLI_STRUCTURED_FRAMES=1` forces frames for any
eligible plan, and `=0` turns them off. Both mechanisms sit behind that
choice: the memo is compiled only into the recording program, which
`recording_forward` builds only for a framed loop, and `instance_trip` runs
only on a framed trip.

## Corpus reach

All 402 corpus models (`corpus_cases(deps/posteriordb)`), point 0 with
`--wa-values`, `STANLI_STRUCTURED_LOOP_DIAGNOSTICS=1`, default settings
otherwise. The diagnostics switch only prints: it does not change which loops
are retained or how they run.

- 9 models retain an `OP_LOOP`: `cm_invgaussian`, `covid19imperial_v2`,
  `covid19imperial_v3`, `ctsem_ctsm`, `ctsem_ctsm_400`,
  `ctsem_ctsm_post4_100`, `iohmm_reg`, `mlumr_survival_relaxed` and
  `mlumr_survival_mspline_relaxed`.
- 1 model records frames: `ctsem_ctsm_post4_100`. It is the only corpus model
  that reaches the memo or frame instances by default.

With frames forced (`STANLI_STRUCTURED_FRAMES=1`) on the same 9 models,
summed over the recordings that `stanli_check` makes for one point:

| model | memo hits | memo misses | frames | instantiated | fell back | refused |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `cm_invgaussian` | 0 | 0 | 202 | 0 | 0 | 0 |
| `covid19imperial_v2`, `_v3` (each) | 0 | 26 | 6,001 | 0 | 0 | 3,182 |
| `ctsem_ctsm` | 156 | 49 | 323 | 191 | 116 | 0 |
| `ctsem_ctsm_400` | 3,192 | 98 | 6,406 | 3,982 | 2,392 | 0 |
| `ctsem_ctsm_post4_100` | 1,984 | 218 | 593 | 278 | 72 | 152 |
| `iohmm_reg` | 0 | 2 | 501 | 494 | 0 | 0 |
| `mlumr_survival_relaxed` | 0 | 202 | 318 | 85 | 67 | 114 |
| `mlumr_survival_mspline_relaxed` | 0 | 0 | 182 | 85 | 62 | 0 |

The memo hits only on the three ctsem models. Frame instances also fire on
`iohmm_reg` and both `mlumr` models once frames are forced, but those models
do not choose frames on their own.

## What they replace on the first gradient

`bench_grad <mir> <data> --prep --parallel-wa=off` runs exactly one gradient.
A temporary counter, removed afterwards, counted the instructions that the
recording program dispatched during that gradient. The 400-subject entry
records frames only when forced.

| model | both on | memo off | instances off | both off |
| --- | ---: | ---: | ---: | ---: |
| `ctsem_ctsm_post4_100` (default) | 1,930,487 | 3,817,230 | 14,039,516 | 122,276,732 |
| `ctsem_ctsm_400` (frames forced) | 626,867 | 1,435,990 | 1,486,418 | 2,295,541 |

On `ctsem_ctsm_post4_100` the two together dispatch 63 times fewer recording
instructions than neither. The count leaves out the work that takes their
place: a memo hit still runs its parameter steps from the trace (382,370
recorded events), and an instantiated frame still runs its parameter kernels.
So the counts show the size of what each mechanism skips, not the time it
saves.

Other counters from the same runs on `ctsem_ctsm_post4_100`:

- Memo: 75 candidates, 953 hits, 32 misses, 88 KB of tables.
- Frame instances: 181 of 195 frames instantiated, 4 fell back. With the memo
  off, 187 are instantiated.
- With the memo off, the sealed frames use 4 programs and 638,725 code words
  instead of 6 and 753,153. Warm gradients run those frames, so the timing
  below measures warm gradients too.

## The audit's claims

- "The data-loop memo only speeds up the first gradient." Mostly right. It
  runs during a recording, which happens on the first gradient of each
  executor and again when a guard fails and the loop records afresh. A
  copied executor that takes over a ready frame tape does not record. Warm
  gradients never reach it.
- "Frame instances have no separate measurement." Partly wrong. The 0.25.0
  entry in `CHANGELOG.md` gives a first-gradient measurement with
  `STANLI_NO_STRUCTURED_FRAME_INSTANCE=1` as the off arm, on the model from
  Stan Discourse thread 41545: 5.33 s to 1.34 s with 10 latent states,
  44.9 s to 8.51 s with 20, and peak memory from 8.95 GB to 9.27 GB at 20.
  No note under `notes/` records the protocol or the raw data, and the corpus
  entry for that model (100 subjects) was not measured.

## The timing that would decide

Not run: the machine was shared. Run it on an idle machine.

**Build.** One shipped-configuration build with the developer switches read,
so that one binary serves all four arms:

```sh
cmake -B build-loops -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ -DSTANLI_DEBUG_SWITCHES=ON -DSTANLI_EMBED_STANC=ON
cmake --build build-loops --target bench_grad --parallel 6
```

The switches are read when a loop's state is built, not per instruction. To
confirm a result on the shipped configuration (switches not read), build a
second tree with one of these one-line changes instead:

- memo off: in `RecordingProgram::compile_data`
  (`runtime/src/structured_recording.inc`), drop the `compile_memo(s)` call;
- instances off: in `structured_loop_forward`
  (`runtime/src/structured_loop.cpp`), set `recorder->instancing = false`.

**Models.**

- `ctsem_ctsm_post4_100` with default settings: the only corpus model that
  reaches either mechanism.
- The same model at the post's own size, 2,000 subjects, if R and the pinned
  ctsem are available: `Rscript tools/gen_ctsem_data.R OUT/ctsem_ctsm_post4_2000.json
  --generator post4 --subjects 2000`. This is the size that a user reported.
- `ctsem_ctsm_400` with `STANLI_STRUCTURED_FRAMES=1`, as a bound on what the
  mechanisms would do for a model that chose frames. It does not choose them
  by default, so it does not count toward the decision.

**Arms.** both on; `STANLI_NO_DATA_LOOP_MEMO=1`;
`STANLI_NO_STRUCTURED_FRAME_INSTANCE=1`; both switches set. The arms are
the same binary, so models that do not reach the code need no controls.

**Commands.** Serial, one thread (`--parallel-wa=off`), fresh processes, five
rounds with the arm order rotated each round. Compile the MIR once:

```sh
P=$(bash -c 'source tools/stanc_embed/provenance.sh && stanc_embed_dir')/stanli-vectorize-probe
$P --vectorize-loops on --output OUT/ctsm.sexp tests/ctsem/ctsem_ctsm.stan
```

Then, for each model's `DATA.json`, run these two processes per arm. The
first measures preparation, the first gradient and peak memory with exactly
one gradient. The second measures warm gradients: 200 ms of warmup, then 20.

```sh
B=build-loops/bench_grad M=OUT/ctsm.sexp D=DATA.json
# both on
/usr/bin/time -l $B $M $D --prep --parallel-wa=off
$B $M $D 20 --parallel-wa=off
# memo off
STANLI_NO_DATA_LOOP_MEMO=1 /usr/bin/time -l $B $M $D --prep --parallel-wa=off
STANLI_NO_DATA_LOOP_MEMO=1 $B $M $D 20 --parallel-wa=off
# frame instances off
STANLI_NO_STRUCTURED_FRAME_INSTANCE=1 /usr/bin/time -l $B $M $D --prep --parallel-wa=off
STANLI_NO_STRUCTURED_FRAME_INSTANCE=1 $B $M $D 20 --parallel-wa=off
# both off
STANLI_NO_DATA_LOOP_MEMO=1 STANLI_NO_STRUCTURED_FRAME_INSTANCE=1 \
  /usr/bin/time -l $B $M $D --prep --parallel-wa=off
STANLI_NO_DATA_LOOP_MEMO=1 STANLI_NO_STRUCTURED_FRAME_INSTANCE=1 \
  $B $M $D 20 --parallel-wa=off
```

For `ctsem_ctsm_400`, put `STANLI_STRUCTURED_FRAMES=1` in front of every
command. Write each switch as its own word: zsh does not split a variable
that holds two assignments, so `env $ARM` with both switches in `ARM` turns
off only the first.

`--prep` prints preparation seconds, parameter count, peak RSS after
preparation and first-gradient seconds. `/usr/bin/time -l` reports the peak
RSS of the whole process, first gradient included. All the ctsem entries use
the same source, so one MIR file serves every data set.

**Record** first gradient, warm gradient, preparation and peak RSS
separately, as medians over the rounds, and report each off arm as a speedup
of on over off (old/new). Values need no check here: `tests/test_ctsem_engine.py`
and the `structured_no_memo` cross-path configuration compare the memo with
its off arm bitwise, and the frame-instance tests in
`tests/test_structured_loop.cpp` do the same for instances.

**What it decides.** With one corpus model in reach, the corpus geomean
cannot move by 2%, so the question is whether the first-gradient time on the
ctsem post #4 model is worth 805 and 1,917 lines. The 41545 thread is a
real user's model, and the time to a first gradient is the time to a first
useful posterior there.

## Limits

- Counts are from arm64 macOS with the build at the top of this note.
- The reach survey runs one point per model. A model that records frames
  only at other parameter values would not show here. Frame selection reads
  the size of the first 8 trips' recording, which depends mostly on the
  loop's shape and data, so this is unlikely.
- The ordinary corpus data is small. Models that would choose frames at a
  larger data size, as `ctsem_ctsm_400` does when forced, are not in the
  corpus at that size.
