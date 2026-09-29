# Decision: the next execution architecture

Historical experiment: its implementation and build targets were removed before
merge. The commands below describe the recorded experiment at commit
`84945f1b`, not tools in the current tree. Results and limitations remain
part of the research record. See the [cleanup audit](2026-09-29-execution-code-cleanup.md).

> **Current decision (2026-09-28):** Native instruction generation, stencil
> JIT, and dispatch-JIT research are tabled at the user's request, informed
> by their earlier negative investigations. The codegen proposals below are
> historical, not active next steps. Native performance is the priority; Wasm
> is a secondary demo. Continue coverage migration and measured improvements
> within the existing graph, register, structured-loop and kernel engines.
> Removing `MirInterp` remains an objective and does not require a JIT.

The user asked to continue until completion or a major design decision. The
[census](2026-09-28-execution-census-implementation.md) and
[eleven-family scalar RNG migration](2026-09-28-scalar-rng-implementation.md)
are implemented. This document marks the first architecture decision; it does
not claim interpreter deletion or a production code generator.

## Recommendation for the next research tranche

Adopt a **shared lowered representation and kernel contract as the design axis**,
then compare native code generation with native-host Wasm before choosing a
backend. Preserve the existing `Program`/`AdjProgram` semantic work initially,
emitting its arithmetic and control directly while calling precompiled Stan
numerical kernels. This is a research direction, not a frozen public ABI.

Two architectures remain viable:

- Browser Wasm plus native code generation for native installations. Native
  generation may use a packaged backend; it need not mean handwritten emitters.
  It must cover arm64/x86-64 and each intended OS's executable-memory policies.
- Wasm as the sole generated format, with an embedded Wasm execution engine and
  host-kernel imports on native installations. Browser Wasm is required under
  either route; the incremental choice is native generation versus an engine.

Neither architecture has won a matched comparison. A packaged compiler backend
and an embedded engine both carry dependency costs; size is unmeasured for both.
The Wasm-to-Wasm import probe below does not measure the native-host boundary.
The earlier provisional preference for two targets was withdrawn following
[Fable's review](2026-09-28-fable-codegen-review.md).

The consequential choice is whether to proceed with the shared generated-code
boundary and a matched backend comparison now, or continue widening existing VM
coverage first. I recommend the bounded comparison now, as the reviewed roadmap
intended, while retaining current execution engines. Do not choose or integrate
a production backend on this evidence. The exact next experiment is below.

## What the probes actually establish

`tools/probe_native_program.cpp` emits a bounded subset of real `Program` and
`AdjProgram` objects into C++. The subset covers add/multiply, copies/constants,
acyclic branches, generated reverse segments and a real Cholesky kernel call.
The host passes a narrow callback bridge; the generated module never reads
`KernelCtx` or C++ container layouts. This is a developer-only comparator,
compiled using `-O3 -ffp-contract=off`, without fast-math. It is excluded from the
default build and is not a JIT shipped to users.

Four cases check values and weighted input adjoints, changing branches, reused
register/checkpoint storage, and signed zero/nonfinite inputs for arithmetic.
The kernel case rejects an invalid Cholesky input and then successfully reuses
its state. Finite values/adjoints require identical bits; NaN classifications
are compared rather than payload bits. Unsupported emitter instructions refuse.
The VM/adjoint is the differential oracle for this probe; this synthetic native
module has not independently been evaluated through a generated CmdStan model.
The production RNG patch has separate independent CmdStan evidence.

Both native timing runs used six alternating pairs (200 ms warmup, 250 ms
samples). The VM library and harness were built in **Release**, as was the
scalar migration. The emitted module used `-O3 -ffp-contract=off`.

| Case | Initial VM / generated median | Final VM / generated median | Final saved time |
| --- | ---: | ---: | ---: |
| 16 arithmetic instructions | 47.6 / 20.9 ns | 47.3 / 20.8 ns | 26.5 ns |
| 256 arithmetic instructions | 768.6 / 551.0 ns | 631.7 / 553.9 ns | 77.7 ns |
| Branch with overwritten live-in | 34.0 / 10.0 ns | 34.8 / 9.9 ns | 24.9 ns |
| Cholesky 2×2 kernel | 154.0 / 151.8 ns | 162.1 / 155.9 ns | 6.2 ns, inconclusive |

Both runs are saved in [the evidence JSON](data/2026-09-28-codegen-probe-results.json),
including MAD and individual samples. The large-chain estimate moved materially
between runs; treat the size of that gain as unsettled. No identical-binary A/A
control was run, so the small kernel difference is inconclusive. Input copying,
adjoint clearing and output seeding are common harness work; absolute deltas
are more useful than ratios.

These are microbenchmarks, not model/sampling speedups. The programs are
hand-built, not extracted from profiled census-selected model regions. What
fraction of a real gradient is removable VM work remains unknown, so the value
of either code-generation architecture for shipped models is unquantified. Generated code retains
the register-buffer layout; a scalar-SSA emitter is an untested alternative.
The external compiler took 256 ms to build all four functions into one dylib.
That cold cost is far too large to assume profitable specialization for an
interactive tiny model. It motivates comparing faster emission strategies; it
does not establish that native generation itself is too expensive. The final
dynamic load took 371 µs in one observation; that excludes compilation.
Charging this entire four-function compile/load cost to one workload gives
illustrative break-even counts of about 9.7 million evaluations for the short
chain, 3.3 million for the long chain, and 10.3 million for the branch. These are
not per-model compile-cost predictions: the compile serves all four functions.
They nevertheless show why this external-toolchain prototype does not establish
acceptable startup at the roadmap's 2,000/20,000 evaluation budgets. Raw initial and final verification
runs are retained in the accompanying evidence JSON.

The browser probe builds the **current** runtime with Emscripten and adds four
exports only to a separate `probe_codegen_wasm` target. The normal `stanli_wasm`
and webR exports are unchanged. `tools/probe_generated_kernel.c` produces a
separate Wasm module with explicit imports for memory, kernel forward and kernel
reverse. An opaque handle owns kernel metadata but also captures borrowed buffer
addresses at creation: **binding and evaluation frame are fused in this
prototype**. The importing runtime owns the allocation arena; caller-owned
value/adjoint buffers must outlive the handle. The proposed separate-frame
interface below is not implemented here.

`tools/probe_generated_kernel.cjs` verified changing-input values/weighted
adjoints against direct calls to the same runtime kernel, rejection and recovery,
and linear-memory growth from 16 MiB to approximately 24.6 MiB while existing
handles/buffer offsets stayed valid. JavaScript views are reacquired after
growth. The module is about 0.5 KB. Initial direct/imported Cholesky-forward
medians were about 74.7/77.1 ns, including the JS entry; the few-nanosecond
difference is not a precise isolated call-cost estimate. The final tiny-module compilation/instantiation observations were 44.5/12.4 µs;
these exclude emitting the bytes and compiling the large runtime. They cannot be
compared directly with the external native compiler's full process time.

The same fresh browser-runtime artifact passed the existing end-to-end smoke
and the new 54-value scalar RNG CmdStan replay (maximum 1 ULP). These runs use
Node's WebAssembly engine. They do **not** establish webR side-module loading,
browser CSP compatibility, hardened native executable-memory permissions, every
platform, or a production ABI. The original Wasm script reseeded output adjoints before each reverse and did
not test consume-and-clear semantics. The post-review bridge now clears output
adjoints; its regression checks that a second unseeded reverse adds nothing.
The initial script also lacked compile-time reporting, which was added for the
final run. These revisions and raw results are recorded separately.

Wasm exception checks establish that both routes
reject and recover, not exact exception type/message equivalence. A stable
interface must specify its error contract explicitly.

## Concrete shared-boundary proposal to evaluate

Keep these operations internal and versioned until the probe covers the intended
hosts. Do not expose current C++ structures or table indices as durable ABI:

1. Bind a kernel descriptor (family/variant, logical shapes, activity, immutable
   metadata) to an opaque handle. Validate all lengths and capture ownership.
2. Supply per-evaluation value, output, adjoint and scratch buffers through a
   frame. Shapes/metadata belong to the binding; RNG/reduction/error state belong
   to the evaluation. Concurrent calls use distinct frames.
3. Execute forward and backward through bound entry points. Preserve forward
   scratch/factorization and checkpoint values until the matching backward.
   Define additive adjoint semantics, aliasing and output-adjoint consumption.
4. Release frames and bindings explicitly. Generated module lifetime must not
   outlive its imported runtime, and bindings must not outlive owned metadata.
5. Return a defined error status/message through the host boundary. The probe's
   cross-module native exceptions are evidence about feasibility, not the final
   error ABI. Preserve print/reject/RNG ordering and recovery.

The prototype implements only a fixed Cholesky binding, using the saved output
factor for reverse. It exercises no kernel scratch, userdata or evaluation-state
contract. Fable's review and all dispositions are linked above. Dynamic shapes, solver
callbacks, recursive regions, RNG/reduction state, concurrency, lifecycle errors
and effectful kernels remain unproven. The kernel contract must cover these;
freezing four prototype exports as a production ABI would be premature.

## Bounded next experiment after the architecture choice

Compare a fast native emitter or packaged optimizing backend against native-host
Wasm on the **same** lowered scalar/branch/kernel programs. Include the identical
Wasm kernel interface in webR and an actual browser. Measure all-in preparation,
first gradient, warm value/reverse, peak/retained memory, compressed and installed
size, and executable-memory/module policy. Include a vector/matrix-dominated
control, larger loops, small/medium models and an identical-binary A/A control for
marginal changes. Compute break-even evaluations rather than relying on warm
throughput. Preserve the shared CmdStan oracle and add a native-generated-model
oracle before promoting the synthetic comparator into a runtime backend.

Use the result to select a backend, revise the shared contract, or keep a
candidate experimental. Existing interpreters remain correctness fallbacks until
coverage and all deployment gates are met. Preparation, initialization,
standalone recursive functions, two container RNG names, broader container forms,
callback logical shapes and general exits remain coverage work regardless of
which target architecture is chosen.

## Reproduction

Native: build `probe_native_program`; run `--emit generated.cpp`; compile that
file with `clang++ -std=c++17 -O3 -ffp-contract=off -dynamiclib` on macOS (use the
platform's shared-library equivalent elsewhere); run `--run` with its absolute
library path. This probe is currently wired for Unix hosts.

Wasm: configure the repo with Emscripten, build `stanli_wasm` and
`probe_codegen_wasm`; compile `tools/probe_generated_kernel.c` with the SDK's
Clang targeting wasm32, `-O3 -ffp-contract=off -nostdlib`, linker flags
`--no-entry --import-memory --export=forward --export=reverse
--export=raw_forward --allow-undefined`; invoke the JS probe with the experimental
runtime JS path and generated module path. The ordinary runtime smoke is
`tests/test_wasm.cjs`. The new RNG oracle can be replayed with
`tests/test_scalar_rng_reference.py tools/wasm_check.sh` and
`STANLI_WASM_MODULE` pointing at the ordinary runtime.
