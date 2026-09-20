# ctsem recording performance: implementation draft

Status: implemented and numerically validated. The ordinary-model timing
and process-memory investigation is deferred at the user's request.

Base: PR391, merged into `origin/main` at
`0daf3b15452d758f290afffe70a1cf8e4759310b`. The working branch includes that
remote default-branch commit.

## Problem and architecture

Numerical frames already reduce ctsem's recorded storage substantially, but
the first gradient still interprets the original control tree and repeats
binding/version bookkeeping for data-only computations. Those costs are
paid while building the replay representation; warmed gradients reuse it.

The draft compiles a temporary recording program after frame admission.
Explicit jumps replace recursive control dispatch. A conservative,
whole-region constancy proof specializes canonical data-only instructions
while preserving the same kernels, dynamic checks, execution order and
control guards. The program belongs to one recording attempt and is
destroyed on success or failure. It adds no preparation pass or persisted
recording-program cache to ordinary models.

The proof rejects effects, custom kernel overrides, parameter-varying
inputs, alternate output writers and other unsupported cases. Original
recording remains available through independent control-program and
data-specialization ablations.

## Measured result

Six alternating fresh-process pairs compare against the completed frame
implementation, with identical benchmark-driver objects and thread settings.

| ctsem rows | First gradient, frames | First gradient, draft | Reduction |
| --- | --- | --- | --- |
| 400 | 2.707 s | 1.963 s | 27.5% |
| 4000 | 26.870 s | 19.239 s | 28.4% |

At 4000 rows, warmed gradients are 500.22 / 499.31 ms. Preparation and
warmed timing intervals include parity. Live allocation bytes and counts
are exactly equal at all measured boundaries. These are incremental gains
over frames; the earlier PR391-to-frames comparison is separate.

Default and forced-frame reference checks each pass 329 models at three
parameter points, covering 1,020,194 values with maximum scaled error
9.38e-13. Four ctsem sizes at three points match all 581 density/gradient
values bitwise against PR391. Targeted ASan/UBSan checks pass. Full CTest
passes 260/262; the two existing signature-generation failures are unchanged.

## Ordinary-model controls

All 316 finite ordinary benchmark cases have bitwise-equal outputs and
identical live allocation bytes/counts. The same three cases are nonfinite
in both revisions. The sole ordinary retained-loop model, `s2_gev`, passes
the independent timing controls.

On 2026-09-20 the user deferred the residual timing/RSS investigation so
ctsem work can continue. Its protocols, measurements and attribution limits
are preserved in [research notes](../../../notes/performance/2026-09-20-ctsem-ordinary-model-controls.md).
Those observations are not considered resolved, and no binary padding or
function-order tuning is included.

## Review and future work

Fable reviewed the profiles, original frame changes, specialized recording
proof and future plans through `claude`, explicitly selecting
`claude-fable-5-1` with its own Read, Glob, Grep and Bash tools. Its
adversarial-test suggestions were implemented, including canonical-kernel
positive coverage, late publication across frame boundaries, parameter
branches, in-place updates, exceptions and retry.

The subsequent [index and iterator experiment](2026-09-20-ctsem-index-recording.md)
implements two further recording specializations. Its execution census
identified fixed-geometry data-only point reads as the largest group of
index calls. These reads use the normal kernel on first execution, then
reuse its descriptor proof while checking current selectors every time.
The existing publication bit holds the proof; no extra cache allocation or
instruction widening is needed. Dynamic geometry and custom kernels retain
the general path, and untaken sites retain their original error behavior.

The updated profile then justified specializing transient iterators with
data-valued bounds and a unique writer. Loop entry still validates and binds;
later iterations only change the workspace value. Both paths are temporary,
independently switchable, and tested against their disabled paths and the
original tree interpreter. Their full semantic checks pass. The final
six-pair comparison reduces the 4000-row first gradient from 19.37 to
15.27 seconds (21.1%), with unchanged live storage and warmed/preparation
timings consistent with parity. The [recording-site notes](../../../notes/performance/2026-09-20-ctsem-recording-sites.md)
preserve the ordinary controls, including one unresolved small RSS signal.

Executor clone reuse needs a separate lifetime and API-order design:
sampling creates clones before chain initialization, so a ready tape is not
always available to share. Variable row shapes and multiple top-level loops
also remain memory-scaling limits. These changes are not implemented here.

The [research log](2026-09-19-ctsem-memory.md) contains protocols, complete
measurements, build identities and artifact locations. The
[Fable review](../reviews/2026-09-20-ctsem-fable.md) preserves its reasoning
and the qualifications applied during implementation.
