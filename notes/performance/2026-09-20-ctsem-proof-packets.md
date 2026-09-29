# ctsem frame proofs and startup reuse

Checked frame-layout reuse reduces N4000 first-gradient time from 14.109 to
12.815 seconds. The paired median ratio is 0.9061 (95% bootstrap interval
0.8946–0.9152). Warm gradients are statistically unchanged: 410.57 to 412.76 ms,
ratio 1.0055, interval 0.9948–1.0108. Retained live allocation is exactly
1,440,895,440 bytes in both variants; median peak RSS falls by 34.5 MB.

Full observations, identities and diagnostics are retained in the
[scorecard](2026-09-20-ctsem-proof-packets.json).

Implementation commit: `f70ba0f7`. This follows checkpoint `1b2ad991` and fetched upstream
`680a4763b256764476bca72b7916ad312c910701`. A final fetch confirms the same
upstream base. The comparison is additional to the solve and recording changes
in the [earlier results](2026-09-20-ctsem-remaining-architecture.md).

## Proof 1: a checked layout can replace canonical encoding

Let E be the canonical encoding of the current recorded Stream and C be a
previously encoded candidate. The matcher certifies execution equivalence:
for every instruction, its kind, site, flags, arity, lengths, saved-value offsets,
positions and reverse membership agree; every resolved value and adjoint operand
agrees with E. Literals are captured afresh in the same order. Local references
must match the current snapshot offset. External references bind current
addresses or adjoint identities, and repeated indices must bind consistently.

Distinct external indices may now resolve to the same address. This preserves
an alias already present in the current execution. Splitting one index into two
addresses refuses. Consequently C and E need not have identical representations,
but execute identical instructions on identical operands in identical order.
Induction over the forward instructions and the reversed backward list preserves
values, adjoints, exception order and in-place undo. The matcher invokes no model
kernel; on refusal it discards tentative bindings and literals and encodes the
intact Stream canonically.

A four-entry MRU cache bounds comparison cost. Its size is a performance choice,
not a correctness assumption. The initial previous-frame-only attempt had zero
ctsem hits and no useful speedup; that negative result is retained. At N400,
391/394 attempted seals match cached layouts; N4000 matches 3,991/3,994. The cache and recording scratch
are released before warm replay. `STANLI_NO_STRUCTURED_FRAME_LAYOUT` restores
canonical encoding; there is no new warm-replay validation pass.

The optional `STANLI_STRUCTURED_CHECK_FRAME_LAYOUT` verifier independently
encodes each hit and compares resolved pointers, normalized adjoint identities,
literal bits, positions and reverse order. It passes the semantic frame suite
and all four target sizes at all three reference points. Direct fixtures pin
both hits and refusals for alias splitting/merging, normalized index changes and
absolute base shifts; existing fixtures cover promotions, dynamic shapes, guards,
retries, zero trips, in-place undo and value-only calls.

## Proof 2: recorded copies can relocate mutable storage

A copied executor first constructs fresh kernel state. The optional
`KernelState::clone_from` hook defaults to refusal. Loop state accepts only a
completed frame tape from the same immutable plan and a successful completed
forward or backward phase. Entry clears eligibility before mutable execution;
a thrown kernel cannot publish a partially evaluated tape to a copy.

The relocation is an isomorphism between disjoint owned allocations:

| Source field | Destination rule |
| --- | --- |
| Frame values, saved old values, literals; loop workspace | Deep copy with identical lengths |
| Frame bindings; import destinations; outputs and targets | Preserve offset in the corresponding copied allocation |
| Adjoint bindings, promotions, output/target identities | Copy numeric indices and shifts |
| Adjoint values and target reduction scratch | Fresh private buffers |
| Site contexts and outer input/adjoint bindings | Construct fresh; bind during next execution |
| FrameCode and code-interning references | Share immutable code |

All pointer classification occurs before publication. Null/empty operands retain
the existing empty semantics; unknown, misaligned or out-of-range addresses
refuse transactionally. Checking a binding base suffices because identical
allocation lengths preserve every access valid in the source program. Import
and output extents are checked explicitly. Range mapping preserves both pointer
equality and inequality, so it preserves alias topology. Adjoint indices resolve
against the new tape and new outer executor.

The clone represents continuing the source's current valid state, including
post-in-place values after a value-only call. Its next forward validates inherited
guards and refreshes inputs and scratch. A guard change discards the inherited
tape and records from fresh state. Reverse starts unready. Recording-only memo,
versions, arenas and contexts from the source are never needed for this replay.

Tests cover source mutation and destruction, unused copy chains, value-only
sources, changed data/guards, concurrent guard-preserving replay over shared code,
forward and backward exceptions, stream sources, automatic frame admission,
destination replay refusal, and explicit clone disablement. An extension-kernel
fixture publishes a foreign scratch pointer and verifies transactional refusal
and an untouched source. The four target sizes also compare copied and freshly
recorded executors bitwise across points 0/1/2/0 with value-only calls between.

Copying a completed frame executor now reuses its recording by default.
`STANLI_NO_STRUCTURED_FRAME_CLONE` retains fresh-copy behavior. The new virtual
method changes the KernelState vtable; all in-tree runtime objects and consumers
were rebuilt. This build has no prebuilt kernel packs deriving from KernelState;
such external packs would require rebuilding.

## Recorded-copy measurements

Six alternating fresh-process pairs use the same binary, with recorded cloning
enabled or explicitly disabled. Both variants first record the source; that
source cost is excluded from the copy-plus-first-gradient boundary below and
retained separately in the scorecard.

| Rows | Copy itself, off → on (ms) | First gradient, off → on (s) | Copy + first gradient, off → on (s) | Paired ratio / 95% interval |
| ---: | ---: | ---: | ---: | ---: |
| 400 | 0.266 → 9.103 | 1.3307 → 0.0431 | 1.3310 → 0.0523 | 0.03933 / 0.03735–0.03964 |
| 4000 | 0.421 → 91.865 | 12.8696 → 0.4182 | 12.8700 → 0.5103 | 0.03972 / 0.03941–0.04003 |

At N4000, recorded-copy startup is about 25.2× faster. Copy construction performs
more work because it allocates and relocates the tape; the first gradient then
replays it. In the two-executor scenario, final live allocation is about 75 MB
lower with cloning enabled. Mutable numerical history remains private.

The single warm call in the N400 startup experiment showed an apparent slowdown
(38.89 → 42.64 ms). One fixed six-pair check using a 300 ms warmup and 1,000 ms
measurement window did not reproduce it: 39.84 → 39.93 ms, paired ratio 0.9949,
interval 0.9894–1.0080. Both the initial single-call observations and this sustained
check are retained. No additional retries were used.

These results require an already recorded source. They do not apply to the
current sampler's unrecorded-source copy boundary. Two-pair cold-source and
small stream-source probes are boundary checks, not speedup claims.

## Proof 3: the current tape has real cross-frame hazards

The [diagnostic patch](2026-09-20-ctsem-flow-proof.patch), applied to the canonical
baseline `1b2ad991` with `git apply --unidiff-zero`, walks operands by role and tracks each owned cell's last reader and
writer in forward execution order. A scalar recurrence has three RAW boundaries;
rebinding its state before each trip removes all three. Its numerical values and
sensitivities also agree with explicit tanh recurrence equations.

At N4000 the target has 3,995 frames, with 3,992 body boundaries crossed by RAW,
WAW and WAR hazards. GEMM site 2060 reads 100 cells written by the preceding
body frame on 1,996 trips. Indexed writes save the previous value before
replacement, and reverse restores it: a reset implemented that way cannot
remove its own physical dependency. The diagnostic also finds 1,295 prefix
adjoint cells accumulated by multiple body frames. Reordering those additions
would violate the bitwise contract in general.

The closure inventory classifies 1,399,573 captured pointers with zero unknowns.
That establishes pointer ownership for the clone mechanism; it does not establish
parallel independence. Some declared dynamic-length call footprints are
conservative. The fixed-size GEMM and indexed-write witnesses are explicit.
Per-site mutable contexts and shared writable workspace present further hazards.
Parallel replay remains deferred until a partition proves private state,
correct carries/resets and the original per-cell adjoint addition order.

## Validation and measurement boundaries

Apple M3 Ultra, Apple clang 21, arm64 Release, existing floating-point options,
one thread, pinned dependencies and saved O0 MIR. Six alternating fresh-process
pairs per target size; 300 ms warmup and 1,000 ms measurement. Intervals are
10,000 paired bootstrap medians with seed 20260920; no observations are removed.
Preparation includes saved-MIR compilation/binding, not Stan source compilation.
The first-gradient interval includes recording and the first reverse pass.
Correctness, builds and profiling run separately from timing gates.

| Rows | First gradient, baseline → selected (s) | First paired ratio / 95% interval | Warm paired ratio / 95% interval |
| ---: | ---: | ---: | ---: |
| 32 | 0.2366 → 0.2444 | 0.9987 / 0.9966–1.0886 | 0.9980 / 0.9906–1.0102 |
| 33 | 0.2456 → 0.2447 | 0.9754 / 0.9483–1.0757 | 1.0010 / 0.9945–1.0093 |
| 400 | 1.4621 → 1.3263 | 0.9078 / 0.8953–0.9411 | 1.0022 / 0.9981–1.0072 |
| 4000 | 14.1089 → 12.8150 | 0.9061 / 0.8946–0.9152 | 1.0055 / 0.9948–1.0108 |

N32/N33 remain streams; N400/N4000 use frames. Retained allocation is identical
for each paired size. Ratios are medians of pairs, so they need not equal the
ratio of marginal medians shown in the time columns. Small stream cases do not
establish a startup benefit.

Fifteen six-pair ordinary-model controls preserve bitwise outputs. Two initial
small slowdown signals triggered one fixed six-pair A/A and A/B follow-up each.
Neither repeats with a raw interval excluding parity: `sw_se` ratio 1.0091,
interval 0.9883–1.0223; `solve` ratio 0.9931, interval 0.9802–1.0149. Identical-binary
noise and adjusted intervals are retained in the scorecard. This does not prove
universal performance parity; no further timing retries were used.

All four target sizes at three points remain bitwise equal to the baseline;
the largest scaled external difference is 3.2102e-13 against the unchanged 1e-9
gate. All 329 reference models pass all three points (1,020,194 values; worst
scaled error 9.38e-13). Final CTest passes 262/264, with exactly the two known
signature model-generation baseline failures. No new exclusion was added.

A separate 20-evaluation diagnostic splits warm time into forward/reverse:
221.48/191.00 ms for baseline and 220.19/193.37 ms for selected. These are
single-process means, not the paired timing gate. Reprofiling the selected
first gradient captures 7,708 samples: 6,010 include `recording_forward`, 1,369
include frame sealing, and 666 include matcher value/adjoint resolution. These
inclusive counts overlap and cover a ten-second recording window. The next
startup hypothesis is reducing repeated operand resolution, with a separate
certificate; this change does not assume a reusable relocation plan.

The [initial proof audit](../../docs/superpowers/plans/2026-09-20-ctsem-proof-review.md)
and [implementation audit](../../docs/superpowers/plans/2026-09-20-ctsem-proof-implementation-review.md)
found no implementation counterexample. The implementation audit's additional
refusal and concurrent-replay tests are included and pass. The Fréchet
replacement remains rejected under the previously recorded non-normal matrix
accuracy evidence. No end-to-end sampling improvement is claimed: normal sampler
copies still start from an unrecorded executor.


## PR preparation verification

The final CI formatting cleanup uses clang-format 22.1.8. A Release rebuild
preserves all 70 runtime archive members byte for byte; only archive metadata
changes. Fresh validation again passes all 329 reference models and the layout
and clone adversaries, with full CTest still 262/264 and exactly the two known
baseline failures. The [PR readiness record](2026-09-20-ctsem-pr-readiness.json)
links the final source hashes and rebuilt archive to the measured implementation.
