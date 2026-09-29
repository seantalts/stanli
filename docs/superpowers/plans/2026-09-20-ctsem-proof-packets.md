# ctsem follow-up proof packets

User objective: continue the deferred architecture work by establishing the
missing proofs and implementing improvements that survive them. Fresh origin
default is `680a4763b256764476bca72b7916ad312c910701`; the clean task branch
already contains it. Starting implementation is `1b2ad991`, with the measured
selected binary from `.cache/ctsem-remaining/selected` as the baseline.

## Evaluator and current state

Preserve bitwise arithmetic, exception/effect order, and transactional refusal.
Use positive and adversarial synthetic frames plus an unrelated retained-loop
canary, then ctsem N32/33/400/4000 and changing points with value-only calls.
Use the unchanged external gate (1e-9), the 329-model reference gate, and full
CTest before integration. The two signature-generation baseline failures
remain known; no new exclusions are authorized.

The immediate stopping artifact is a checked-layout prototype with hit/refusal
counters and differential tests, followed by a two-pair N400 feasibility probe.
If it removes recording work without numerical or warm-replay changes, proceed
to six alternating fresh-process target pairs and ordinary controls. Provisional
selection criterion: a first-gradient interval below parity with no confirmed
ordinary slowdown; report small uncertain signals rather than declaring zero
cost. Builds/tests and timing runs remain separate. Record prep, first/warm
gradient, forward/reverse, live allocation and peak RSS separately; no sampling
claim without sampling. No fixed speedup magnitude was requested.

## A. Checked frame-layout reuse

Claim: most later frames can instantiate an existing program without rebuilding
its word vector, hash, external-pointer interning maps or normalized-index pool.
The previous profile attributes material first-recording work to encoding and
hashing. Reusing existing code requires less than proving that source-level
control flow always has a fixed layout: validate the actual recorded frame.

Proposed generic certificate:

1. Consume the current Stream in order, matching every instruction kind, site,
   flag, arity, length, old-value offset and instruction word count.
2. A local value operand must have the same offset in the freshly constructed
   arena snapshot. An external operand binds the current pointer; every repeated
   use of that external index must bind the same pointer. Null/empty operands
   preserve the existing null encoding.
3. An adjoint operand must preserve the local offset or bind its current full
   identity (id, shift, promotion), with repeated-index consistency.
4. Every relative gather/scatter position must equal the referenced immutable
   position list. Absolute shifts still bind the current base pointer.
5. Literals are captured afresh in the same index order. Targets, imports,
   value copying, version compaction and ownership remain on the existing path.
6. Match exactly the whole program, including active/backward instruction
   structure. Candidate code originates only from the established encoder.

The proof is by instruction induction: equivalent pointer/adjoint operands,
positions and literal values imply identical calls and updates in the same
order. Matching never invokes a model kernel. Failed matching discards only
the new frame's tentative bindings and literals, then runs the old encoder on
the intact Stream. Exceptions and model effects are therefore not replayed.
No assumption of value independence or alias independence is introduced.

Near misses: changed instruction path, changed arity/shape, differing normalized
positions, local-to-external transitions, repeated binding indices that now
refer to different values or adjoints, promotions, empty ranges, and a mismatch
after partial binding. Cover initial/final frames, guard re-recording, retry,
in-place updates, copies, value-only calls and independent executors.

Bounded continuation: start with the immediately previous frame as candidate;
measure hits and comparison cost before considering a small layout cache.
Do not add model fingerprints or a new warm-replay check. If pointer resolution
dominates, keep a broader relocation-plan proof as a separate hypothesis.

## B. Refine the parallelism obstruction

Counterproposal: remove false dependencies caused by coarse frame ownership,
rather than make serial encoding faster. The earlier census found references
to previous frames, but did not identify read/write overlap or whether those
references are mathematically live at reset boundaries.

Immediate experiment: classify cross-frame references by instruction kind,
site/operand, value versus adjoint role, interval and writes. Determine whether
the observed connections are real carries, conservative aliases, or immutable
data. A legal parallel partition must additionally prove private writable
workspace, disjoint frame writes, and a stated parameter-adjoint reduction
order. No parallel execution follows merely from absent unknown pointers.

Near miss: a genuine scalar recurrence must retain its cross-frame edge;
an overwrite-before-read reset may remove only the dependence it kills.
The stopping artifact is an operation-level dependency witness and explicit
next proof obligation, not a thread executor based on a coarse census.

## C. Relocating recorded clones

Keep as a separate startup mechanism. The required proof is pointer closure
over owned frame values, old values, literals and workspace, with immutable
program sharing and fresh contexts/adjoints. Unknown pointers must refuse
transactionally. Source mutation, value-only calls and independent clones
must not share mutable storage. The actual sampler clones before recording,
so a pointer proof alone does not establish a sampling benefit.

Current priority: implement/test A and refine B's diagnostic while Fable 5.1
reviews the proof obligations. Reassess C using any reusable relocation facts
established by those experiments.

## Proof results and revised scope

The previous-frame-only matcher had zero target hits and no useful first-gradient
benefit. A bounded four-entry MRU cache now matches 391 of 394 attempted seals at
N400. Candidate selection affects only comparison cost: the complete certificate
is checked for every hit, including reverse instruction membership. Distinct
external indices may safely bind the same current pointer; the certificate is
execution equivalence, not identical encoding. A diagnostic canonical encoder
compares resolved operands, literal bits, index positions and reverse order on
hits. It passes the frame suite and all four target sizes at three points.

A new operand-role diagnostic sweeps forward reads/writes in execution order
with a last writer/reader for each owned cell. The scalar recurrence canary has
three RAW boundaries; rebinding the state at each trip removes all three. On
N4000 the canonical target retains 3,992 RAW, WAW and WAR boundaries. For example,
GEMM site 2060 reads 100 cells produced in the previous body frame on 1,996 trips.
The sweep also sees 1,295 prefix adjoint cells accumulated by multiple body
frames. These are hazards in the current physical execution, not a proof that
no future mathematical partition exists. Dynamic-length call footprints remain
conservative; the reported fixed-size GEMM and indexed-write witnesses are
explicit. In-place saves read the overwritten value and reverse restores it.
Parallel replay therefore remains deferred pending private workspace/contexts,
carry/reset treatment, and preservation of each adjoint cell's addition order.

The pointer closure census classifies all 1,399,573 captured pointers at N4000.
This supports implementing packet C as a separately ablatable mechanism:

- `KernelState::clone_from` defaults to refusal and is offered only after the
  copied Executor binds fresh state. The loop implementation accepts a completed
  frame tape from the same immutable plan; streams and incomplete/failed tapes
  remain fresh. A successful forward or backward establishes eligibility; entry
  clears it so exceptions cannot publish partially evaluated tapes.
- Copy every frame's values, undo values and literals, plus workspace. Share only
  immutable FrameCode. Build a sorted source-range to destination-range map and
  relocate every frame binding, import destination, output and target. Unknown
  or out-of-range pointers refuse before publication. Identical allocation
  sizes preserve the valid source program's operand extents and alias topology.
- Copy numeric adjoint identities and promotion tables, allocate private adjoints
  and target reduction scratch, and retain freshly constructed contexts. The
  next forward rebinds evaluation resources and validates the copied guards.
  Publish using swaps only after closure succeeds; reverse remains unready.
- Tests cover copy chains before first use, value-only sources, source mutation
  and destruction, changed data/guards, failed replay, explicit disablement and
  concurrent copies. All four ctsem sizes pass changing-point comparisons between
  recorded copies and fresh-recording copies, with interleaved value-only calls.

This is an already-recorded-copy startup optimization. The normal sampler copies
an unrecorded executor; its boundary is unchanged and no sampling benefit is
claimed. The Fréchet replacement remains rejected under the earlier non-normal
matrix accuracy evidence. No tolerance was relaxed to admit a new fast path.

A recorded copy preserves the source's current valid execution state, including
post-in-place values after a value-only call. The next forward has the same
semantics as continuing the source. Copying a completed frame executor now
reuses that recording by default; callers requiring fresh recording can set
`STANLI_NO_STRUCTURED_FRAME_CLONE` before copy construction.

The new virtual method changes the KernelState vtable. The in-tree loop,
reduction and island states were rebuilt with the runtime and all consumers.
This build uses no prebuilt kernel packs deriving from KernelState; external
packs would need rebuilding against the changed header.


## Final disposition

A and C are selected. Six fresh-process pairs reduce N4000 first-gradient time
14.109 → 12.815 s (paired ratio 0.9061, 95% interval 0.8946–0.9152), with unchanged
retained allocation and no established warm change. Recorded copy plus first
gradient falls 12.870 → 0.510 s (same binary, clone reuse off/on; paired ratio
0.03972, interval 0.03941–0.04003). Copy itself grows from 0.421 to 91.865 ms.
The source recording cost is excluded from that explicitly stated boundary.

Two small ordinary-model signals did not repeat with intervals excluding parity
in the one fixed A/A and A/B confirmation. An apparent N400 single-call warm
clone penalty also did not repeat in the one fixed sustained-window check:
ratio 0.9949, interval 0.9894–1.0080. All observations remain in the scorecard.
Cold-source and small stream-source copies retain fresh-recording behavior.

All 329 model references pass all three points; all target differential and
changing-point checks are bitwise. Full CTest remains 262/264 with the same two
signature-generation baseline failures. The final refusal fixtures include a
foreign captured pointer, a backward exception, stream sources, automatic frame
admission and destination replay disablement. Concurrent copies preserve guards
and explicitly assert that neither thread re-records. No further external review
was requested after the user's instruction to stop asking Fable.

B stops at a stronger, validated dependency witness. Parallel replay, changing
the sampler's clone boundary, a reusable relocation plan, and replacing the
matrix-exponential pullback still require separate evidence. The current
post-change profile leaves operand resolution and recording dispatch as concrete
startup hypotheses. See [proofs and measurements](../../../notes/performance/2026-09-20-ctsem-proof-packets.md).
