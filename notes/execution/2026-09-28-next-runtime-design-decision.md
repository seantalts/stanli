# Runtime boundary and continued native-performance work

Native performance is the priority. Native instruction generation, stencil JIT,
dispatch-JIT and further backend comparisons remain tabled. Nothing proposed
here emits machine code or changes the installation toolchain requirement.

## What the completed slices establish

Fixed-shape functions with early exits now run through the existing register
compiler. Callback values/gradients, effects and independent model outputs are
validated. Eligible standalone real-valued functions now reuse compiled plans,
with full shape/integer specialization keys and private execution buffers.
Standalone cache churn is real: unbounded specialization can cost more than
interpretation, so the cache uses measured admission rather than recompiling
every new signature indefinitely. These are coverage changes within existing
engines, not a new execution backend.

Neither slice removes differentiation tapes inside numerical kernels. Removing
MIR interpretation and removing local Stan Math tape construction are different
work items. The former avoids syntax/name/container traversal; the latter can
improve an already compiled model's repeated numerical work.

## The architectural boundary

The register compiler assumes fixed storage ranges. General runtime-sized
results, changing local shapes, arbitrary call frames/recursion and cold-path
partial-environment evaluation exceed that contract. Raising register limits or
retaining more specialized copies does not solve this boundary. The public
standalone API already exposes some of these behaviors, so deleting the MIR
interpreter must preserve them rather than declare them unsupported.

The existing roadmap already orders these priorities. Continue its native
hot-path work now; the second option would reorder the roadmap, not unblock
the current work:

1. **Continue native hot-path coverage first (recommended for the stated
   performance priority).** Profile real models for nested-tape cost, then move
   the leading scalar/transform/matrix/density pullbacks into proven shared
   kernels. Expand callback argument geometry with explicit shape and storage
   adapters. Continue fixed-shape coverage without a new general execution
   representation. Keep the remaining MIR entry points visible and tested.
   This advances measured native throughput but leaves interpreter deletion as
   a later architecture project.

2. **Start the general value-runtime project now.** Compile the remaining
   value-only MIR surface into reusable typed instructions over runtime-owned
   values, with logical shapes and function call frames. Start with standalone
   fallback calls and preparation, retaining the existing flat register and
   graph paths for admitted hot computations. This attacks the deletion gates
   directly, but adds a language-runtime representation and preparation/size
   costs before a broad native performance payoff has been demonstrated.

Within option 2, prefer a separate general-value representation behind the
shared compiler/registry over adding dynamic-tag/shape checks to every existing
flat-register instruction. That preference is provisional: benchmark a bounded
prototype before choosing a production architecture. Both remain dispatching
C++ evaluators; eliminating MirInterp would not eliminate all instruction
interpretation. No JIT is needed or proposed.

## Concrete first experiment if option 2 is selected

Implement a test-only evaluator for scalar/vector real values, integer control,
dynamic vector allocation, returns and direct function calls. Use the existing
builtin registry and numerical kernels. No new numerical algorithms or public
ABI. Prove API storage order and integer result mirrors at the boundary; retain
exact print/reject sequencing. Start with double values only, and leave
callback autodiff and inverse parameter transforms explicitly unclaimed.

Evaluate one function with alternating return lengths, one bounded recursive
helper, a preparation loop with changing temporary extents, and a fixed-shape
canary. Compare with the MIR interpreter and independently recorded CmdStan
outputs. Measure compile/prepare time, first and warm calls, rotating signatures,
peak/retained memory and binary growth. Keep the fixed graph/register engine
unchanged and check that its startup/throughput does not regress.

The experiment ends with a reviewed decision artifact: adopt a bounded next
stage only if semantic coverage is correct and the generic execution costs have
a credible measured path to the project's native performance goals. Otherwise
retain the current interpreter and continue option 1. Do not enable a prototype
by default merely because it removes an interpreter counter.

Fable correctly identified that the user's native-performance priority already
selects option 1. This is not an approval blocker or a reason to stop. Continue
with a real-model nested-tape profile, retaining option 2 as a later architecture
project. Reconsider the ordering only if measured mandatory MIR routes become
the bottleneck for a user-visible workload.

The next evaluator uses the shared corpus through representative ordinary
small/medium fixtures and recorded models that actually exercise candidate
kernels. Attribute forward/backward time with the existing profiling machinery;
choose a target from measured tape share, not the number of remaining names.
A provisional success gate is at least 10% improvement in the selected target's
warm gradient, with no resolved preparation/first-gradient or ordinary-canary
regression and no new dependency. This is an experiment-selection threshold,
not a user-approved numerical or regression budget. Preserve the existing
numerical gates and require a measured whole-inference direction before claiming
a user-facing win. Report binary and retained/peak memory costs.

The deletion gates still include the census of preparation, initialization,
folding/admission probes, write-array and retained callbacks; the integer-result
mirror/overflow contract; runtime-shaped results (dynamic_result), recursion
(descend), and changing temporary extents (sized) in the standalone API tests.
Compiled standalone call timings are not model-gradient throughput evidence.
The branch helper's first-call overhead is reported in the implementation note,
alongside the warm gain and cache-churn correction. A future general-runtime
prototype needs explicit startup/size budgets and a portable installation plan
before production adoption; no Wasm development is proposed in this slice.
