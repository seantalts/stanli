# BridgeStan ABI facade and sampler strategy

Date: 2026-08-10 (revised after review, same day)
Status: draft for review
Branch: feat/bridgestan-facade

## Goal

Let external MCMC samplers use stanli models, and set up an
evidence-based path for choosing stanli's default sampler. Both goals
are served by one piece of work: stanli implements the BridgeStan C
ABI, the interface those samplers already speak.

## Context

stanli ships one universal shared library and compiles no code on the
user's machine. Sampling today is in-process
`stan::mcmc::adapt_diag_e_nuts` behind `Model.sample()`.

Two external samplers motivate this design:

- **nutpie** (PyMC developers, Rust): mature NUTS with strong online
  mass-matrix adaptation (the Fisher-divergence approach originated
  here). Loads Stan models as BridgeStan shared libraries through its
  Rust bridgestan bindings.
- **walnutpie** (Flatiron Institute, C++20 header-only, v0.0.1):
  implements WALNUTS (arXiv 2506.18746), which adapts the leapfrog
  step size within a trajectory to handle varying curvature. Runs
  concurrent chains with R-hat auto-stopping. Its Python layer passes
  a BridgeStan library path into C++, which dlopens it and shares one
  `bs_model` handle across one `std::jthread` per chain.

Both consume the same interface: a per-model shared library exporting
`bs_*` symbols (the BridgeStan C ABI). Rather than vendoring either
sampler, stanli exposes that interface. Dependencies then point
upstream: samplers evolve on their own schedules, stanli tracks one
slow-moving ABI, and every other BridgeStan client (the bridgestan
Python/R/Julia packages, future samplers) gets stanli models for
free. BridgeStan's premise is "Stan model as a differentiable shared
library, given a C++ toolchain"; stanli makes that toolchain-free.

## Compatibility stance

"BridgeStan compatible" means: every call either behaves exactly as
reference BridgeStan documents, or fails with -1 and a clear error
message. No call silently serves a different quantity than the flags
request. The unsupported set in v1, all erroring honestly:

- `bs_param_unconstrain`, `bs_param_unconstrain_json` (no inverse
  transforms in stanli),
- `bs_log_density_hessian`, `bs_log_density_hessian_vector_product`,
- density flags other than `propto=true, jacobian=true`,
- `bs_param_initialize` with non-NULL JSON or `jacobian=false`.

Implementing inverse constraint transforms would unlock the first and
last of these together; that is recorded as the natural follow-up, not
part of v1.

## Non-goals (this design)

- No vendored sampler and no change to `sample()` or its contract.
- No inverse constraint transforms.
- No JS/WASM exposure of the facade (dlopen has no browser analog).
- No Windows facade in v1 (see Risks; the runtime itself still builds
  there).

## Design

### ABI surface

Implement every symbol in bridgestan.h at the version walnutpie
vendors, in a new TU `runtime/src/bridgestan_abi.cpp` (C++17, no new
dependencies), exported from the universal shared library alongside
the existing `stanli_*` API. Every symbol exists because clients bind
the full set at load time (ctypes and dlsym both fail hard on a
missing symbol). Failure returns are exactly -1 with a heap-allocated
message freed by `bs_free_error_msg`, per the reference header.

### Model identity: the lib pair

BridgeStan bakes the model into the library; stanli has one universal
library. Bridge the gap with per-model instantiation:

`Model.bridgestan_lib(dir)` writes two files and returns the first:

1. a clone of the stanli runtime library, named for the model
   (`clonefile` on APFS, `FICLONE` reflink on Linux where the
   filesystem supports it, byte copy otherwise), and
2. a sidecar manifest `<libstem>.stanli.json` holding the transformed
   MIR (the s-expression stanc already produced for this model), the
   model name, the runtime build id, and the Stan source text for
   diagnostics.

The sidecar carries TMIR, not just source, so `bs_model_construct`
never invokes the embedded stanc: construction is manifest read,
lowering, and executor build against the caller's data JSON (inline
JSON or a file path, both per the ABI). This avoids per-clone OCaml
runtime startup entirely; the stanc bridge's caml_startup once_flag
and its thread-affinity assumption stay confined to the main library
the Python package loaded. The pair already pins the exact runtime
binary, so serialized TMIR adds no version coupling beyond what the
clone created; the manifest's runtime build id makes a mismatched
pair fail loudly instead of misbehaving. Exposing TMIR text to the
helper needs a small `stanli_*` C API addition (compile source to
MIR text without building a model).

The cache directory is content-addressed: the pair's stem is a hash
of (runtime build id, TMIR). Two different programs sharing a model
name cannot collide, a stanli upgrade cannot serve stale clones, and
"idempotent" means exact: same runtime, same program, same pair.

`bs_model_construct` locates its own library path with `dladdr` on
the address of a TU-local, hidden anchor function (not an exported
`bs_*` symbol, so symbol interposition cannot misattribute the
clone), then reads the sidecar next to it. Distinct file paths give
distinct dlopen instances, so multiple models coexist in one process;
the integration test asserts each clone reads its own manifest.

### Construction seed and transformed data

`bs_model_construct(data, seed)` uses the seed the way reference
BridgeStan does: it seeds RNG calls in transformed data. stanli's
transformed-data evaluation runs through the MIR interpreter, which
already has `_rng` machinery on the write_array side; construction
wires the same function hook into the TD hooks with a
construct-seeded RNG. The existing `stanli_model_new_from_stan` path
takes no seed, so the facade uses a new internal construction entry
that does. A TD-RNG model is part of the conformance suite; same
source, data, and seed must produce the same model as reference
BridgeStan produces for its own RNG conventions (values compared
where the conventions coincide; where they cannot, the test pins
stanli's behavior explicitly).

### Concurrency

One `bs_model` handle is evaluated from many threads: walnutpie runs
one jthread per chain over a shared handle, nutpie calls from a Rust
thread pool. The handle owns an **ExecutorPool**: a mutex-guarded
free list of executor clones with a scoped per-call RAII lease (pop,
evaluate, push back). No thread-local executor ownership: a
persistent lease would pin clones across thread churn and raise
lifetime questions when the model is destroyed before a caller
thread exits. The AD stack stays thread-local and thread-lifetime
(one `stan::math::ChainableStack` per evaluating thread, the same
requirement `run_nuts_chains` documents); only executors move through
the pool. The uncontended mutex is noise next to a gradient; if the
parity bench measures contention on sub-microsecond-gradient models
under many chains, the recorded fallback is a lock-free freelist,
adopted on evidence only. The pool is a named component because a
future in-process sampler needs exactly the same thing.

Multi-threaded clients require a `STAN_THREADS` build, which is what
the released wheels are (verify during implementation and record in
the compatibility notes). Single-threaded clients work on any build.

### RNG ownership

BridgeStan makes `bs_rng` a separate, caller-owned, explicitly
non-thread-safe object created per thread. stanli's current
write_array RNG is model-owned: `stanli_wa_seed(model)` stores state
that `stanli_wa_row(model, ...)` consumes. Wrapping that as-is under
a shared `bs_model` would make concurrent `bs_param_constrain` calls
race on one GQ stream.

Refactor first, then wrap: the write_array core (graph executor and
WaInterp both) takes an RNG state parameter per evaluation. The
existing `stanli_wa_seed`/`stanli_wa_row` C API keeps its contract as
a thin wrapper holding one model-owned state; `bs_param_constrain`
passes the state inside the caller's `bs_rng` handle. This refactor
is a standalone commit with its own tests before any facade code
consumes it.

### Semantics on stanli

- `bs_name`, `bs_model_info`: model name from the manifest; info
  reports stanli version, the pinned stan-math version, the
  BridgeStan ABI version implemented, and the build flags that affect
  numerics (`-ffp-contract=off`, STAN_THREADS), mirroring the kind of
  provenance reference BridgeStan puts there.
- `bs_param_num(include_tp, include_gq)`: sliced from the write_array
  column layout. Lowering records the params/tp/gq section boundaries
  so all four flag combinations answer correctly.
- `bs_param_names`: comma-separated, from the write_array names,
  sliced the same way.
- `bs_param_unc_names`: declaration-derived indexed names in
  declaration order (`a.3`, `b.2.3`), matching reference BridgeStan's
  enumeration, which the conformance suite pins. Lowering retains the
  per-declaration unconstrained sizes and dimensions to generate
  them. The current `q.N` placeholder does not conform and is not
  used here.
- `bs_log_density(propto, jacobian)` and `bs_log_density_gradient`:
  pool lease, then the executor's forward or forward+reverse pass.
  Supported flags: `(propto=true, jacobian=true)` exactly, the
  instantiation stanli graphs bake in and the one both motivating
  samplers request. Any other combination returns -1 with an error
  naming the limitation. Serving a plausible number for flags the
  graph cannot honor would be worse than refusing.
- `bs_param_constrain(include_tp, include_gq, rng)`: the write_array
  path with the caller's `bs_rng` state, sliced by the recorded
  section boundaries.
- `bs_param_initialize(json, rng, init_radius, max_tries, jacobian)`:
  v1 supports `json == NULL` with `jacobian == true`: draw uniformly
  from `[-init_radius, init_radius)` on the unconstrained scale using
  the caller's `bs_rng`, check the log density is finite, retry up to
  `max_tries`, -1 on exhaustion, matching the reference semantics.
  Non-NULL JSON requires constrained-to-unconstrained conversion
  (inverse transforms) and returns -1 with an error saying exactly
  that; `jacobian == false` is the density-flag limitation above.
- `bs_set_print_callback`: real plumbing, not a stub. The runtime
  gains a message sink abstraction; `OP_PRINT` (which today writes
  straight to stdout from `runtime/kernels/message.cpp`) and reject
  messages route through it. Default sink is stdout; the facade
  installs the client's callback and serializes invocations with a
  mutex, per the reference contract. Ordinary stanli use keeps the
  stdout default through the same sink, so there is one printing
  path.
- Exceptions anywhere (lowering failure, rejected point) become -1
  with a message; a rejected point in the gradient is an error return
  the samplers already treat as a rejection, mirroring
  BridgeStan-on-Stan behavior.

### Python surface

- `Model.bridgestan_lib(dir=None) -> Path`: writes the pair
  (content-addressed as above; defaults to a per-user cache
  directory) and returns the library path.
- `stanli.bridgestan_model(model, data=None, **kw)`: optional sugar
  returning a `bridgestan.StanModel` bound to the pair. Imports
  bridgestan lazily; bridgestan is not a stanli dependency.
- `stanli.nutpie_model(model, data=None)`: shim constructing nutpie's
  `CompiledStanModel` directly, since nutpie has no public
  prebuilt-library entry point. Documented as best-effort against
  nutpie internals; the durable fix is a `library_path=` parameter
  upstream, which is left as a separate decision.
- Docs: a "bring your own sampler" section with end-to-end walnutpie
  and nutpie examples (each about three lines).

The R binding needs no work: R users go through the bridgestan or
sampler R packages against the same lib pair.

### Default sampler strategy

The user intent on record: stanli's default should be the best
general-purpose MCMC sampler available, not necessarily the current
`stan::mcmc` NUTS. Constraints on any default:

- in-process and toolchain-free (a default cannot ask users to
  install a second package),
- a deterministic fixed-budget mode for reproducibility and testing,
- viable across targets, including single-threaded WASM, or the
  default becomes per-target.

The facade is the measurement instrument for that decision. It runs
candidate samplers against corpus models with identical gradients, so
ESS/second, warmup robustness (funnels, multiscale posteriors), and
failure modes are comparable directly against the built-in NUTS.
Measure first; vendor second.

If walnuts wins on the bench: vendor walnutpie's headers (MIT,
header-only, Eigen-only) behind a single C++20 TU that reuses
ExecutorPool, with bitwise identity against facade-driven walnutpie
runs at fixed budget as the acceptance test. nutpie is Rust and is not
a candidate for in-process vendoring in a C++/WASM runtime; its
adaptation ideas already appear in walnuts. The browser caveat stands
regardless: walnutpie hard-requires `std::jthread`, so single-threaded
WASM keeps the current NUTS unless upstream grows a sequential mode.

None of that work is in this design's scope; this design only ensures
the bench exists and the pool component is shared.

## Testing

RED-GREEN throughout.

1. **C++ unit tests** (direct link, no dlopen): every bs_ function
   against small models; error paths (bad data, rejected points,
   every unsupported flag combination and function, mismatched
   runtime build id in the manifest); section-boundary slicing for
   all four param_num/constrain flag combinations; unconstrained name
   generation for constrained types (simplex, cholesky factor,
   bounded vector); `bs_param_initialize` retry and exhaustion; a
   thread test where N threads hammer `bs_log_density_gradient` on
   one handle and results match a single-thread baseline bitwise;
   concurrent `bs_param_constrain` with distinct `bs_rng` handles
   reproducing each handle's sequential stream.
2. **dlopen integration test**: build a real pair via the Python
   helper, load it the way walnutpie does (dladdr sidecar discovery
   only exercises through a real dlopen), construct, evaluate, and a
   two-models-loaded-at-once case asserting each clone reads its own
   manifest.
3. **Differential conformance against reference BridgeStan**: CI
   compiles a small set of models with real BridgeStan (CI has a
   toolchain; users never need one) and drives both libraries through
   the bridgestan Python client: parameter names and counts (all flag
   combinations), unconstrained names, log density and gradient at
   shared points, constrain output including TP and GQ, NULL-JSON
   initialization behavior, transformed-data seeding, per-handle GQ
   RNG streams, print callback delivery and serialization, and error
   returns for the unsupported set. This is the oracle that catches
   agreeing-on-the-wrong-semantics bugs, which testing stanli against
   its own API cannot. Include one TD-RNG model and one model with a
   print statement.
4. **Sampler parity oracle** (harness, local first, optional CI job):
   walnutpie at fixed budget and fixed seed on models from the
   bitwise-verified set, run twice: once on a BridgeStan-compiled
   model, once on the stanli pair. Same header code, same RNG stream,
   bitwise-identical gradients; the draws must match bitwise, with
   any mismatch treated as a bug until explained. nutpie gets the
   same harness shape if its draws prove run-to-run deterministic.
   This is the end-to-end check on top of the conformance suite, not
   a substitute for it.

## Risks and open questions

- **ABI drift**: BridgeStan's ABI moves slowly but moves. Pin the
  version, expose it via the version globals (clients warn rather
  than fail on mismatch), and let the conformance suite catch drift.
- **RNG stream conventions**: reference BridgeStan's TD and GQ
  streams come from Stan's service RNG conventions; where stanli's
  streams cannot coincide, the conformance test documents and pins
  the divergence rather than leaving it unstated.
- **Windows**: `dladdr` needs a `GetModuleHandleExW` analog and the
  clone step a CoW-less copy. Deferred; the facade compiles out on
  Windows in v1.
- **Copy cost**: on filesystems without CoW the pair costs a full
  library copy (about 22 MB) per model in the cache dir. Acceptable;
  the content-addressed helper reuses an existing pair.
- **Thread-safety of wheels**: multi-threaded clients need
  STAN_THREADS wheels. Believed true of released wheels; verify and
  document.
- **Pool contention**: per-call mutex on sub-microsecond-gradient
  models under many chains could measurably contend. Not
  pre-optimized; the parity bench measures it, lock-free freelist is
  the fallback.

## Phasing

1. Prerequisite refactors, each standalone with tests: caller-owned
   RNG through the write_array core; message sink for print/reject;
   lowering metadata (tp/gq section boundaries, unconstrained
   declaration layout); TMIR-text C API entry.
2. Facade core: bridgestan_abi TU, ExecutorPool, construction path
   with seed, C++ unit tests.
3. Python helper (content-addressed pair) + dlopen integration +
   differential conformance suite.
4. nutpie shim, docs, sampler parity harness.
5. (Separate effort, after benchmarking) default-sampler decision and
   possible walnutpie vendoring.
