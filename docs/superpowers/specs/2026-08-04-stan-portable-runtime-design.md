# stanli: a portable Stan runtime

Status: approved in discussion 2026-08-04. Decisions below record that discussion.

## 1. Goal

A single shared library ("the runtime") that takes a compiled Stan model
representation plus data and produces posterior draws, with no C++ toolchain,
no LLVM, and no compilation of any kind on the user's machine. Distributable
as ordinary PyPI wheels and a thin CRAN package with a fetched payload.

Decisions fixed during design:

- Coverage: full replacement for the current C++ path on day one. Every model
  stanc3 accepts must run.
- Performance: up to 3-5x slower gradient evaluation than today's -O3 compiled
  model is acceptable at first release. Time-to-first-sample is the metric
  being optimized.
- R distribution: thin CRAN shim package, binary payload fetched on install or
  first use (the pattern the CRAN torch package uses). PyPI: normal binary
  wheels via cibuildwheel.
- Deliverable: standalone project vendoring pinned stanc3 and stan-math.
  Upstreaming is a later conversation.
- stanc3 stays the compiler frontend (OCaml, distributed as the single static
  binary it already is). No parser work.

## 2. Non-goals (v1)

- GPU / OpenCL. The opencl branch of stan-math is excluded.
- Distributed execution (MPI).
- Beating current per-gradient performance.
- Upstream-ready patches to stan-dev repositories.
- A stable public C API for third parties (internal ABI only, versioned).

## 3. Architecture

Three artifacts:

1. **stanc3 (vendored, patched)**: gains a backend that lowers its existing
   MIR to a serialized *structured op graph* instead of C++. Runs at model
   "compile" time, which is now milliseconds of OCaml, not minutes of clang.
2. **The runtime DLL**: graph loader, arena allocator, forward/reverse
   executor, op table, constraining transforms, RNG, samplers (NUTS/HMC
   from stan services, compiled in), and data/draw I/O. C ABI. No LLVM.
3. **The kernel library**: precompiled stan-math instantiations behind
   `extern "C"` entry points, built once per target in CI by a real C++
   compiler at -O3, linked into the DLL. Six targets: linux x86_64/aarch64,
   macos arm64/x86_64, windows x86_64/aarch64.

### 3.1 The structured op graph

stanc3's MIR already represents control flow structurally (IfElse, While,
For). The backend preserves that: the graph is a sequence of ops where
control flow is a region-holding op (`COND`, `LOOP`, `WHILE`), not a traced
flattening. Consequences:

- No tracing, no re-tracing, no divergence between trace and semantics.
- `reject()` is an op that unwinds the executor (longjmp-style, C ABI safe).
- User-defined Stan functions lower to subgraphs; higher-order functionals
  (reduce_sum, ode_*, integrate_1d, map_rect) receive a C callback that
  re-enters the executor on a subgraph.
- Shapes are symbolic in the serialized graph (vector[N]); the loader binds
  them to concrete sizes at data-load time and sizes the arenas once.

The op graph is the stable internal contract. Later engines (op fusion, a
JIT) plug in beneath it without changing the compiler or the packaging.

### 3.2 Execution model

Load time: bind shapes, allocate one value arena and one adjoint arena
(flat doubles), allocate per-op scratch for stashed partials, resolve each
op to a function-pointer pair in the op table.

Per gradient: write parameters into the arena, run ops forward (each op
computes its value and, where applicable, stashes partials in scratch),
seed the output adjoint, run ops backward accumulating adjoints. The only
runtime-variable tape state is a branch/iteration counter per control-flow
op. Steady-state sampling allocates nothing.

### 3.3 The heterogeneous op table

Every op presents the same interface (forward, backward over descriptor
structs). Two implementations coexist:

- **Native ops.** Double-only kernels with no autodiff types anywhere.
  Produced two ways:
  - *Recorder kernels* for the ~157 prim/prob density files that route
    partials through make_partials_propagator: a custom scalar type (rvar)
    plus one new partials_propagator specialization makes the unmodified
    stan-math templates deposit value and partials into caller buffers.
    Proven by spike (section 5).
  - *Hand-ported vjp rules* for the small set of matrix/structural
    functions where a native adjoint is worth writing (cholesky_decompose,
    multiply, mdivide_left, log_sum_exp, softmax, gp_exp_quad_cov, etc.).
    posteriordb measurement puts the hot set at roughly 15 functions.
- **Legacy ops.** For every remaining signature: a generated wrapper that
  opens stan::math::nested_rev_autodiff, promotes inputs to var, calls the
  existing unmodified rev/fun (or prim) function, runs grad(), copies
  adjoints out as doubles. Correct by construction because it is today's
  code path. Expensive per call but within the 3-5x budget for ops that do
  real work, and each one disappears from profiles as it gets a native port.

This resolves the central tension: full math coverage on day one (legacy
wrappers are mechanically generated from stanc3's signature table) with a
bounded initial porting effort (approximately 170 files) and an incremental,
profile-driven migration path that is never a release blocker.

Every native port is validated against its own legacy wrapper in-process on
identical inputs. The differential-testing oracle is built into the
architecture.

### 3.4 Autodiff strategy

Reverse mode: the op graph is the tape. Native ops stash double partials
forward and contract them backward; legacy ops delegate to a nested var
tape scoped to the single call. No global var tape, no per-scalar vari
allocation in steady state.

Forward-over-reverse (fvar<var>, needed by laplace_marginal and friends):
v1 covers it entirely through legacy wrappers instantiated at fvar types.
Inefficient but correct. A native second-order story is deferred.

### 3.5 Kernel generation

A generator (OCaml, living beside stanc3's signature table so it cannot
drift from the typechecker) emits one C++ TU per signature:

- Recorder TU where the target is a partials_propagator-based density.
- Legacy TU otherwise.

Arguments cross the ABI as descriptors: {double* data, int64 len/rows/cols,
kind}. Runtime shape dispatch replaces the compile-time scalar/container
instantiation lattice, which is what keeps the kernel count linear in the
signature count instead of combinatorial.

Known wrinkle (found in spike): partials edge index is the propagator
operand position, not the Stan argument position (integer outcomes are not
edges). The generator derives a per-function edge map from the stan-math
source, verified by a build-time test per kernel against the var path.

### 3.6 Distribution

- Payload = runtime DLL (kernels linked in) + stanc3 binary, versioned by
  (stan version, target triple), hosted on GitHub releases / CDN, shared by
  all interface packages.
- PyPI: six wheels via cibuildwheel; the wheel contains the payload
  directly (wheels have no meaningful size ceiling for this: target
  10-20 MB compressed).
- CRAN: shim package under the size norm; downloads the payload on first
  use into the user cache directory, torch-style. Offline escape hatch via
  an environment variable pointing at a local payload.
- Python and R bindings speak only to the C ABI; no dynamic linking against
  interpreter internals, so a model crash cannot take down the host session
  (server-mode isolation can be added later without redesign).

## 4. Error handling

- Domain errors inside stan-math (check_positive etc.) throw C++
  exceptions today; kernel wrappers catch at the ABI boundary and convert
  to an error code plus message buffer. The executor maps that to the same
  semantics CmdStan has (rejected proposal during sampling, hard error
  during data load).
- reject() lowers to an op returning a sentinel that unwinds cleanly.
- The ABI is exception-free and thread-safe per model instance (one arena
  set per chain; kernels are stateless).

## 5. Evidence from spike (2026-08-04, macOS arm64)

Location: scratchpad spike/ (rt_recorder.hpp, spike.cpp, multi.cpp).

- 142 non-blank lines implement the recorder: rvar scalar, five trait
  registrations, three ops_partials_edge specializations, one
  partials_propagator specialization. Zero stan-math lines modified.
- 27/27 outputs bitwise identical to the var path across normal_lpdf
  (all-params and data-observations), student_t_lpdf, gamma_lpdf,
  poisson_log_lpmf, beta_lpdf.
- Object size for one normal_lpdf kernel at -O3: 7,536 bytes text as
  recorder vs 29,464 bytes as var (3.9x smaller).
- posteriordb (120 models): 33 of 181 rev/fun functions referenced by name;
  identifier scan misses operators, so the true count is somewhat higher.

## 6. Testing strategy

- Unit: every generated kernel gets a build-time differential test against
  the var path (bitwise where achievable, else <= 2 ULP with justification).
- Executor: hand-built graphs with gradients checked against stan-math var
  evaluation of the same expression, in-process.
- Integration: posteriordb corpus compiled through the stanc3 backend;
  log_prob and gradient differential-tested against CmdStan on identical
  parameter vectors; sampling smoke tests with fixed seeds.
- Native-port gate: a native op replaces its legacy wrapper only with a
  passing differential test and a benchmark showing improvement.

## 7. Risks

1. Functional callback path (precompiled reduce_sum/ode calling back into
   the executor across the C ABI) is unproven. Spiked early in milestone 1.
2. Legacy wrapper overhead for fine-grained ops (scalar exp in a loop via
   nested tape) could blow the 3-5x budget on pathological models. Mitigant:
   the hot scalar ops are exactly the easiest native ports; port them first.
3. fvar-typed legacy instantiation may hit template compilation issues at
   generator scale. Mitigant: the current C++ path compiles these today;
   worst case is generator special-casing.
4. Windows toolchain behavior for the recorder (MSVC vs clang-cl) untested.
   CI matrix from milestone 1.
5. Sum-ordering differences may prevent bitwise parity on some kernels;
   the test policy explicitly allows documented ULP bounds.

## 8. Milestones

- **M1: spine.** In-memory op graph + executor + recorder kernels for ~20
  common signatures + legacy wrapper generator (proof for a handful of
  functions) + hand-built model graphs. Exit: eight-schools and a GLM
  produce log_prob + gradient matching stan-math var evaluation; NUTS
  sampling via stan services against the executor's log_prob_grad.
- **M2: compiler.** stanc3 MIR-to-graph backend + serialization + loader.
  Exit: majority of posteriordb runs unmodified; corpus-wide gradient
  differential test green.
- **M3: coverage + packaging.** Full signature sweep through the generator,
  functional callback path, CI payload matrix, PyPI wheels, CRAN shim.
  Exit: 120/120 posteriordb; pip install to first sample on a clean machine
  with no toolchain.
- **Ongoing: migration.** Profile-driven native ports replacing legacy
  wrappers, each gated by differential test + benchmark.
