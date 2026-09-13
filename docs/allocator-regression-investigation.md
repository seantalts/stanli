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

Delivery requires distinguishing a harness correction from a runtime fix,
preserving all original evidence, updating the PR/report, and running relevant
checks for any production changes. No default promotion while unexplained
regressions or required integration gates remain.
