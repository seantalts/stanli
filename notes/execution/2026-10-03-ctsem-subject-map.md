# ctsem and OP_REGION_MAP: a per-subject map

Status: design, not started. ctsem already samples faster than CmdStan on the
corpus data (40 subjects x 8 rows: 1.84 vs 3.37 ms per gradient), so this is
about scaling, not a regression.

## What ctsem's loop is

All the work is one loop over data rows in transformed parameters
(`tests/ctsem/ctsem_ctsm.stan`, `for(rowx in 0:ndatapoints)`), not a loop over
subjects. The model block only adds `ll = sum(llrow)`.

- Row 0 is a population pass (`si == 0`). It computes the system matrices
  (`PARS`, `LAMBDA`, `DRIFT`, `DIFFUSION`, ...), the discrete-time
  transition `eJAx` and `discreteDIFFUSION`, and stores `pop_*` copies.
- Rows are grouped by subject. `T0check` counts rows within a subject and is
  reset when `subject[rowi]` changes. At a subject's first row the filter
  state is re-initialised: `rawindparams`, `indparams`, `T0MEANS`, `state`,
  `etacov`.
- Later rows advance the Kalman filter: an integration loop
  `while(intstepi < dt)` and a finite-difference loop over `JAxfinite`, then
  `llrow[rowi] += multi_normal_cholesky_lpdf(...)`.
- Many matrices are recomputed only when something says so:
  `if(si==0 || sum(whenmat[k,...]) > 0 || statedep[k] || dtchange ...)`.
  Otherwise the value from an earlier row, possibly an earlier subject or
  row 0, is reused.
- With `dosmoother` a backward smoothing pass runs at each subject's last row.

Today the map refuses the loop ("body has no target increment"; it also
assigns `state`, `etacov`, `prevrow` and others declared outside), and the
structured loop refuses it too ("assigns an unsized local: PARS"). It runs
unrolled.

## Why a per-subject map is plausible

Two things keep subjects from being independent as written, and data can
remove both:

1. Cached matrices cross subject boundaries. Whether they are recomputed
   depends on `whenmat`, `statedep`, `continuoustime` and `dtchange`. All but
   `dtchange` are data, and `dtchange` depends only on `time`, which is data.
   With those conditions folded, each read of a cached matrix in a subject
   either follows a write in the same subject or sees the value row 0 left.
   That is checkable after folding: a reaching-definitions pass over the
   unrolled subject body, where the only definitions allowed to reach from
   outside are row 0's.
2. The inner loops are data-bounded: the integration loop's trip count comes
   from `dt` and `maxtimestep`, the finite-difference loop from `JAxfinite`,
   the smoother from `dosmoother` and the subject's row count. With the data
   folded they unroll.

## Shape of the change

- **Segmented map.** A new map form for a `for` loop whose iterations fall
  into data-defined segments (here, runs of equal `subject[rowi]`), where
  each segment is one map iteration and the rows inside a segment run in
  order within that iteration. Row 0 becomes a prologue computed once per
  evaluation; its results are live-ins for every segment.
- **Segment classes.** Segments with the same length and the same folded
  data decisions share one compiled program, like data-class specialization
  for LNR (`dec`). Segment lengths vary across real panels, so the number of
  classes needs a cap and a fallback.
- **Outputs beyond the target.** The loop writes `llrow[rowi]` and the model
  adds `sum(llrow)` later. The map needs per-iteration outputs scattered into
  an outer container at distinct indices, or a rewrite that recognises
  `llrow[rowi] +=` followed by `sum(llrow)` as a target accumulation.
- **Privatisation.** Variables declared before the loop but written before
  they are read in every segment (`state`, `etacov`, `rawindparams`, ...)
  are private to the segment, not carried. The independence rule changes
  from "declared inside the body" to "no read in a segment is reached by a
  write from another segment, except row 0's".

## Costs and risks

- Body size. One subject is eight unrolled rows, each row a few thousand
  instructions after folding; lane tiles hold every row's registers.
  Recompute mode or tile recompute may be needed.
- Exactness. Folding the recompute conditions must not change which value a
  read sees. The reaching-definitions check makes that a proof, not a
  heuristic; anything it cannot prove keeps today's lowering.
- Effort: segmented iteration, outputs to containers, privatisation, and the
  definitions check are each a substantial piece. Several weeks.

## First step

Measure before building: per-gradient time, preparation time and peak memory
at 400, 2000 and 4000 subjects against CmdStan. The 2026-09-18 numbers (stanli
3.5x slower, 2.6x the memory at 4000 rows) predate the map and the region
fixes, so they may no longer hold. Build only if a gap remains at sizes users
run.
