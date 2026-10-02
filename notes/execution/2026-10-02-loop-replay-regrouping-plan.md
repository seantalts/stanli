# Structured-loop replay that survives branch flips, then batched execution

Status: plan, not implemented. Scope: `OP_LOOP` only (`runtime/src/structured_loop.cpp`,
`runtime/src/structured_frames.inc`, `runtime/src/structured_recording.inc`). No
change to graph lowering, regions, or other ops.

## Problem

The structured loop records one forward pass and then replays it. The replay is
specialized to the branch decisions taken during recording: each `GuardIf`
stores the decision, and if any observation's decision differs on a later
evaluation the whole recording is discarded and the loop is re-recorded through
the slow interpreted path. A re-record costs roughly 30x a replay.

At a fixed parameter point the recording always holds, which is all our
benchmarks measured. Under NUTS the parameters move, observations near a
threshold change branch, and for several cogmod families nearly every gradient
re-records:

Per-gradient cost during sampling (`stanli_run`, seed 1, 50 warmup and 50
draws, one chain, wall time over total leapfrog steps; both runs take the same
number of leapfrog steps). "loop" is the default structured loop; "regions" is
`STANLI_STRUCTURED_LOOPS=0`. Short runs include fixed startup costs.

| family | loop | regions |
| --- | ---: | ---: |
| gamma | 485 us | 852 us |
| weibull | 468 us | 848 us |
| logstudent | 449 us | 812 us |
| invgamma | 937 us | 978 us |
| invweibull | 1492 us | 935 us |
| logweibull | 1693 us | 996 us |
| bisa | 2474 us | 1621 us |
| loggamma | 3548 us | 1742 us |
| exgaussian | 16122 us | 1446 us |
| geg | 26954 us | 2695 us |
| lognormal | 31447 us | 4800 us |
| exwald | 44722 us | 5304 us |

At a fixed point the same models run at 200 to 900 us per gradient on the loop.

Where the recording holds, the loop beats per-observation runtime-control
regions (gamma, weibull: about 1.8x). Where it does not, regions win by up to
10x (exgaussian, geg). The compile-time choice between the two cannot know
which regime a model is in, so the fix belongs in the loop's replay.

## Phase 1: re-record only the observations that changed branch

Build on the frame tape (`FrameTape`, one `LoopFrame` per outer-loop iteration,
`FrameCode` shared between frames with the same layout). Frames already have
separate ownership, relative operands and offset-based adjoints; the stream's
pooled absolute pointers and single snapshot make replacement much harder.
Keep stream replay for loops whose recording holds.

When frame k's guard disagrees with its recorded decision, roll back the
frame's partial writes, re-execute iteration k alone through the recorder, and
replace frame k. Iteration k's code is matched against existing codes or added.
Over a run the codes converge to the paths the data takes, and a flip costs one
interpreted iteration.

Review findings that shape the first version:

- Re-executing one iteration needs more than numeric inputs. Replay discards
  the recorder's state after sealing, so the repair must rebuild bindings and
  versions (adjoint identity, owner, constancy, aliases), transient workspace
  handles, memo generations, nested-loop generations and cell provenance, and
  reset the compiled recorder's cursors and flags.
- Independence is not "no promotions". Scalar recurrences, integer counters,
  aliases and memo results can cross iterations without one. Admission needs a
  read/write analysis over every path, untaken arms included, plus the
  existing exclusions for effects and RNG.
- Replacing a frame must replace its ordered slice of target terms and output
  adjoints atomically. The first eight iterations are sealed as one frame under
  automatic selection, and an outer `break` or a changed `while` condition
  changes the remaining frame sequence.
- Adjoint storage for replaced frames needs recycling or bounded compaction;
  appending forever grows the vector the backward pass zeroes.

First version, narrowed to what exgaussian and geg need: fixed-count `for`
loops that only add to the target, with read-only live-ins, fully initialized
iteration-local temporaries, no memo dependence between frames, and no effects
or escaping containers. Scalar execution only. Anything else keeps today's
whole-loop re-record.

Before building, measure how many iterations change branch per gradient during
sampling, not only how often the whole loop re-records.

## Phase 2: execute each code group as a batch

Frames sharing a code took the same path, so they execute the same instruction
sequence. Lay a group's values out with one contiguous lane per cell, then run
each instruction once over the whole group:

- Elementwise kernels (arithmetic, `exp`, `log1p`, comparisons, `log_mix`)
  run once with length equal to the lane count.
- Inputs bound outside the frame are either the same pointer in every lane
  (broadcast) or per-lane addresses (gathered into a lane buffer per
  evaluation; data-only sources can be gathered once).
- Densities need their per-lane form: one log density per lane, and a backward
  that takes one adjoint per lane. Density kernels already have an elementwise
  mode (variant bit `0x40`, `densities_impl.hpp`), with its own scratch layout
  and activity-mask rules.
- Some kernels are scalar-only (`OP_EXP`, structured comparisons), and packet
  math or internal reductions can change rounding. Batch only kernels on an
  explicit list proven lane-equivalent.
- Kernels with matrix or structured arguments run per lane.
- Guards: compute the condition for the whole group, then move any lane whose
  decision differs out of the group and handle it with the Phase 1 re-record.

Exactness: every lane computes the same scalar functions as today, so
per-observation values are unchanged. Target terms are summed in iteration
order as today. Adjoints that several lanes add into one shared cell (a
broadcast parameter) must keep every original addition in order: reverse
frames, reverse instructions within a frame, added directly to the existing
accumulator. One subtotal per lane or per instruction is not bitwise. Shared
partial overwrites need their write and restore ordering too; exclude them at
first. Batching also changes validation and exception order, needs private
kernel contexts per batch, and its packing, lane migration and ordered
gradient buffers can eat the dispatch savings, so measure them separately.

Ceiling, from a sampled gamma gradient at a fixed point: the loop is 79% of
the gradient, about half of all gradient time is per-instruction dispatch, and
the math functions are about a sixth. Batching should take the loop from about
190 us to 60-80 us.

## Measurement protocol

Every loop-versus-regions or before/after claim uses sampling: `stanli_run`
with a fixed seed, per-gradient cost from wall time over total leapfrog steps,
and identical leapfrog counts as the check that both runs followed the same
trajectory. Fixed-point gradient timings are secondary and labelled as such.
CmdStan sampling times on the same data are the reference.

## Review

Codex (gpt-6-astra) reviewed this plan on 2026-10-02; its findings are folded
into the sections above. Original questions:

1. Can `Execution` run one outer iteration from a restored iteration-start
   state, given how `seal_frame`, versions and promotions work? What state
   besides imports, pre-loop values and the loop variable must be restored?
2. Is "no promotions other than target terms" a sufficient independence test?
   What carried state could it miss (in-place updates, memo cells, integer
   locals, RNG)?
3. Frame adjoint offsets: is appending a fresh adjoint region for a replaced
   frame safe with promotion links and output adjoints?
4. Phase 2 adjoint accumulation into shared cells: is buffering per-lane
   contributions and reducing in reverse iteration order enough for bitwise
   agreement with the scalar replay, including imports that are partly
   overwritten inside the loop?
5. Are there kernels whose length-1 call and lane-batched call differ in
   results (internal reductions, packet math, saved scratch layouts)?
