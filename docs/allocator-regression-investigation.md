# Allocator regression investigation

2026-09-13. User authorized investigating the regressions after PR #362's
rollout measurements. Starting point `1e5f396a`, clean, ten commits ahead of
`origin/main` (`7bbda72c`), zero behind. Preserve those commits and the separate
dirty research worktree. Defaults remain SYSTEM / alignment OFF. No regression
allowance, allocator tuning, Stan Math/Eigen changes, or auto-merge is implied.

## Hypotheses and first stopping experiment

1. **Writable-buffer placement / false sharing.** The native timing harness
   constructs all `Chain` objects and Eigen gradient outputs on its main thread.
   Production clones executors on the caller but constructs the sampler and its
   working vectors inside workers. A compact allocator can place independent
   tiny outputs on the same cache line. Prediction: allocating only the gradient
   buffer in its consuming worker removes cross-worker output-line overlap and
   improves the small-model parallel loss without changing any allocation/free
   code in the timed loop. Record output, result-scalar and parameter addresses.
2. **Allocator fast-path/TLS/shared metadata cost.** Linux's dlopen-safe dynamic
   TLS or allocator-internal sharing may impose overhead. Prediction: the loss
   persists with independent worker-allocated state, or occurs in pure
   worker-local malloc/touch/free loops too. Do not substitute initial-exec TLS
   or remove foreign-pointer ownership checks to manufacture a win.
3. **Other persistent state or code layout.** If gradient relocation alone is
   insufficient, constructing the complete test Chain in its owning worker is
   a separate discriminator. This is not production equivalence: production
   still clones executors on the caller. If that alone helps, locate the exact
   shared state before changing either the runtime or the benchmark contract.

The architectural counterproposal is to avoid cross-thread construction of
per-worker scratch/output buffers, rather than tuning the allocator. This
changes lifetime/placement outside the hot loop, not model semantics. Test it
before investigating lower-level allocator implementation changes.

First bounded Linux experiment on both original manylinux architectures:
unchanged SYSTEM/private runtime implementations, three test-only placements
(`main`, `gradient`, `worker`), all with full-byte checks at eight changing
points and graph-dimension equality. Cases are normal 8 at one/four workers,
Eight Schools/four, normal 1024/four and large normal/four. Five balanced rounds,
two processes per slot and round, three timed blocks, and an identical alias
for all six configurations: 600 timing processes per platform. Baseline-only
150 ms calibration is frozen before timing. Retain every process, snapshot,
address map, controls, command and binary/source/input identity. No concurrent
build/test on a timing host. First inspect reproduction and address overlap;
do not interpret a non-reproducing case as proof that a cause was fixed.

Stop this first experiment at its complete scorecard. If gradient relocation
removes overlap and recovers the loss while pure allocator paths remain
unchanged, verify the production allocation boundary before correcting the
benchmark and remeasuring broader consumers. If losses remain, collect a
separate bounded allocation-only/profile experiment, without expanding the
normal-corpus screen repeatedly. Pure allocation and profiling runs are queued
diagnostics, not already measured facts.

Apple ARM next: a two-by-two allocator/alignment ablation, focused on large
normal/eight workers and normal 1024/four as control, with both identical-binary
aliases, before assigning its historical combined loss to either change. Freeze
the design and builds before looking at timing results. A benchmark-placement
finding on Linux is not automatically an explanation for Apple ARM.

The Apple ablation is frozen to normal 262144/eight workers and normal 1024/four
workers, all four configurations and their identical aliases, five balanced
rounds, two processes per slot, three blocks: 160 timed processes. The same
test-only source is used for all builds, retaining main-thread state placement
to isolate the two build settings. Baseline-only 150 ms calibration precedes
timing; all four configurations must match full gradients and graph dimensions.
The additional generic `placement.py --design` input expresses these frozen
variants and paired comparisons; it does not alter the running Linux design.

## First checkpoint and bounded TLS/profile continuation

Native run [34753964261](https://github.com/seantalts/stanli/actions/runs/34753964261)
completed 600 timed + 30 verification processes on each Linux architecture,
all full bytes and graph dimensions equal. On x86, normal 8/four workers
remains 0.722x with main placement, 0.707x with worker-owned gradient storage,
and 0.695x with completely worker-owned test state. Eight Schools and normal
1024 also lose with all three placements. The proposed main-thread placement
explanation is rejected for the persistent x86 deficit. ARM's old normal-8
loss does not reproduce even in the unchanged main placement (1.121x); this
is unresolved non-reproduction, not a demonstrated fix.

The Apple two-by-two finishes 160 timed + eight verification processes with
full-byte agreement. Large normal/eight workers has private/SYSTEM throughput
ratios 0.933 with alignment OFF and 0.937 with alignment ON. The latter loses
five/five rounds. Normal 1024/four workers improves 1.327/1.338x. The small
alignment effects have noisier controls (some A/A shifts around 3%); the
combined large-case ratio is 0.971 in this new binary set. These are attribution
measurements, not replacements for the original screen/confirmation.

Next discriminating stage: same native Linux runtime and fully worker-owned
diagnostic state, SYSTEM versus private local-dynamic TLS versus private
pthread TLS. This is an upstream-supported backend selected only on the
measurement target by a deferred project hook; no shipping source/default
change and no initial-exec TLS substitution. Preserve both compiled allocator
flag sets. Cases: normal 8 at one/two/four workers, normal 1024/four, and large
normal/four, five rounds, two processes per slot, all three aliases: 300 timed
processes/architecture. The two-worker case helps distinguish physical-core
scaling from the x86 runner's four logical CPUs on two SMT cores.

Before timing, attempt two user-mode sampling profiles (SYSTEM/private original
TLS, normal 8/four, worker placement) in the isolated Linux CI container. Retain
failures if the runner disallows perf; unavailable profiling is not evidence
about the hotspot. Profiling and builds never run concurrently with timings.
Stop at the complete TLS scorecard: if the backend change recovers the loss,
validate ownership/late-load behavior and broader workloads before integrating
it. If not, use available profiles to select the next cause; do not tune TLS
variants repeatedly until a favorable sign appears.

For the Apple residual, collect one eight-second native stack sample per
SYSTEM/aligned and private/aligned process on normal 262144/eight workers,
64,000 repetitions, with full end-of-run snapshot agreement. Use the already
frozen ablation DSOs; no rebuild or option changes. These sampled durations
are diagnostic only, never added to the uninstrumented timing scorecard.

Delivery requires distinguishing a harness correction from a runtime fix,
preserving all original evidence, updating the PR/report, and running relevant
checks for any production changes. No default promotion while unexplained
regressions or required integration gates remain.

## TLS checkpoint and host-startup discriminator

Native run [34754559037](https://github.com/seantalts/stanli/actions/runs/34754559037)
passes 300 timed + 15 verification processes per architecture. On x86 normal
8/four, dynamic TLS is 0.682x and pthread TLS 0.694x, both negative five/five.
The TLS substitution does not recover the loss and will not be integrated.
ARM remains positive with either backend. This x86 runner is Intel Xeon 8573C,
not the previous placement runner's AMD EPYC 7763; both expose two cores/four
SMT threads. Do not pool their absolute timings.

Both user-mode Linux profiles succeeded. The long profiled runs do not show
a large allocator CPU-time increase, and both include OpenBLAS thread-server
samples from the host's NumPy import. In the uninstrumented x86 short runs,
normal 8/four's frozen 97,112 repetitions produce roughly 33 ms SYSTEM blocks,
not the nominal 150 ms calibration target. Some private block times decrease from
roughly 190 to 120 to 80 ns/gradient across the three samples. This warrants
a startup-contention discriminator; it is not yet proof of the cause and
profiled elapsed times are not a replacement benchmark.

Next fixed design uses unchanged SYSTEM/private native code and worker
placement, crossing the two allocators with (a) original NumPy thread defaults,
(b) OPENBLAS_NUM_THREADS=1 before import, and (c) original defaults with 500 ms
settling after import/dlopen and before native execution. All six have aliases.
Three cells: normal 8/one worker, normal 8/four, normal 1024/four, with fixed
500,000 / 100,000 / 30,000 repetitions respectively (not short-block-derived
calibration). Five balanced rounds, two processes/slot, three blocks: 360 timed
+ 18 verification processes per architecture. Record host-thread CPU counters
before/after settling and native execution, cgroup limits/counters and exact
environment overrides outside timing. Retain both DSOs. Stop at that scorecard:
if controlling host startup recovers the loss, correct the benchmark boundary
and validate long warmed runs; do not present it as a runtime allocator fix.
If not, retain the negative and return to runtime attribution.

Apple profiles pass complete snapshot equality. Most samples remain inside
normal_lpdf; private madvise appears in 620 of approximately 45,616 worker
samples, predominantly under _mi_os_reuse / MADV_FREE_REUSE. Pinned mimalloc's
purge delay is 1,000 ms (arena multiplier four), not 10 ms. The reuse syscall
is independent of that purge option, so simply disabling purging is not a
source-supported fix for this hotspot.

## Apple singleton-allocation fallback discriminator

Architectural alternative to allocator-internal tuning: keep small allocations
private but let the system allocator handle mimalloc's singleton-sized malloc
requests on Apple. The fixed threshold is mimalloc 3.5.1's default size-class
boundary (512 KiB), not a model-size cutoff selected by a sweep. Test only
malloc first: calloc/aligned APIs and ownership-preserving realloc stay as
before. A deferred project hook replaces only the diagnostic DSO's shim;
shipping sources and both upstream dependencies remain unchanged.

Compare the frozen aligned SYSTEM/private DSOs with this aligned diagnostic
using normal 262144 at one/eight workers and normal 1024/four as the preserved
small-allocation canary. Five rounds, two processes per slot, three blocks,
all three aliases: 180 timed + nine verification processes. All graph and
gradient bytes must agree. Retain the shim/configuration and binary identities.
If the fallback recovers the large-vector deficit, broaden the affected-size
and ownership matrix before shipping; if it does not, do not threshold-sweep.
This is a scoped mitigation test, not proof that direct syscall time accounts
for the entire original regression.
