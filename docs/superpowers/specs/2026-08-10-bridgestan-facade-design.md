# BridgeStan ABI facade and sampler strategy

Date: 2026-08-10
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

## Non-goals (this design)

- No vendored sampler and no change to `sample()` or its contract.
- No `bs_param_unconstrain`: stanli has forward constraint transforms
  only. The functions exist and return a clear error.
- No JS/WASM exposure of the facade (dlopen has no browser analog).
- No Windows facade in v1 (see Risks; the runtime itself still builds
  there).

## Design

### ABI surface

Implement every symbol in bridgestan.h at the version walnutpie
vendors, in a new TU `runtime/src/bridgestan_abi.cpp` (C++17, no new
dependencies), exported from the universal shared library alongside
the existing `stanli_*` API:

- `bs_model_construct`, `bs_model_destruct`, `bs_free_error_msg`
- `bs_name`, `bs_model_info`
- `bs_param_num`, `bs_param_unc_num`, `bs_param_names`,
  `bs_param_unc_names`
- `bs_log_density`, `bs_log_density_gradient`
- `bs_param_constrain`, `bs_param_initialize`
- `bs_rng_construct`, `bs_rng_destruct`, `bs_set_print_callback`
- `bs_major_version`, `bs_minor_version`, `bs_patch_version`
  (globals matching the pinned BridgeStan version)
- Unsupported, present with honest error returns:
  `bs_param_unconstrain`, `bs_param_unconstrain_json`,
  `bs_log_density_hessian`, `bs_log_density_hessian_vector_product`

Every symbol exists because clients bind the full set at load time
(ctypes and dlsym both fail hard on a missing symbol). Errors follow
the BridgeStan convention: nonzero return, heap-allocated message
freed by `bs_free_error_msg`.

### Model identity: the lib pair

BridgeStan bakes the model into the library; stanli has one universal
library. Bridge the gap with per-model instantiation:

`Model.bridgestan_lib(dir)` writes two files and returns the first:

1. a clone of the stanli runtime library, named for the model
   (`clonefile` on APFS, `FICLONE` reflink on Linux where the
   filesystem supports it, byte copy otherwise), and
2. a sidecar manifest `<libstem>.stanli.json` holding the Stan source
   text and model name.

`bs_model_construct(data_json, seed)` finds its own library path with
`dladdr` on one of its own symbols, reads the sidecar, compiles the
source through the embedded stanc, lowers it, and builds executors
against the data JSON the caller passed (inline JSON or a file path,
both per the ABI). Distinct file paths give distinct dlopen instances,
so multiple models coexist in one process. The pair is relocatable
because the manifest embeds the source text rather than a path.

### Concurrency

One `bs_model` handle is evaluated from many threads: walnutpie runs
one jthread per chain over a shared handle, nutpie calls from a Rust
thread pool. The handle owns an **ExecutorPool**: a mutex-guarded free
list of executor clones with a thread-local lease, and one
`stan::math::ChainableStack` per leasing thread (the same requirement
`run_nuts_chains` documents). The pool is a named component because a
future in-process sampler needs exactly the same thing.

Multi-threaded clients require a `STAN_THREADS` build, which is what
the released wheels are (verify during implementation and record in
the compatibility notes). Single-threaded clients work on any build.

### Semantics on stanli

- `bs_name`, `bs_model_info`: model name from the manifest; info
  reports stanli and BridgeStan-ABI versions.
- `bs_param_num(include_tp, include_gq)`: sliced from the write_array
  column layout. Lowering records the params/tp/gq section boundaries
  so all four flag combinations answer correctly.
- `bs_param_names`, `bs_param_unc_names`: comma-separated per the ABI,
  from the write_array names and the `q.N` unconstrained names.
- `bs_log_density(propto, jacobian)` and gradient: pool lease, then
  the executor's forward or forward+reverse pass. The flags are
  accepted and ignored: stanli graphs bake in the Jacobian and each
  density's propto choice, and the value served is the `lp__` CmdStan
  reports. Both samplers pass `(true, true)`, which matches. The
  divergence for other flag values is documented, not an error, to
  match what the graph can serve.
- `bs_param_constrain(include_tp, include_gq, rng)`: the existing
  write_array path, sliced by the recorded section boundaries. The
  `bs_rng` handle wraps the write_array RNG stream, seeded with the
  seed `bs_rng_construct` receives; deriving per-chain seeds is the
  caller's job under this ABI.
- `bs_param_initialize`: per the pinned header's semantics (verify at
  implementation against BridgeStan's reference).
- `bs_set_print_callback`: stored; stanli has no print statement
  support today, so nothing is ever emitted through it.
- Exceptions anywhere (stanc failure, lowering failure, rejected
  point) become error returns; a rejected point in the gradient is an
  error return the samplers already treat as a rejection, mirroring
  BridgeStan-on-Stan behavior.

### Python surface

- `Model.bridgestan_lib(dir=None) -> Path`: writes the pair
  (idempotent for the same model and dir; defaults to a per-user cache
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
   unsupported functions); section-boundary slicing for all four
   param_num/constrain flag combinations; a thread test where N
   threads hammer `bs_log_density_gradient` on one handle and results
   match a single-thread baseline bitwise.
2. **dlopen integration test**: build a real pair via the Python
   helper, load it the way walnutpie does (dladdr sidecar discovery
   only exercises through a real dlopen), construct, evaluate, and a
   two-models-loaded-at-once case.
3. **Conformance**: the bridgestan Python client driving the stanli
   pair on a few corpus models, values bitwise against stanli's own
   API.
4. **Sampler parity oracle** (harness, local first, optional CI job):
   walnutpie at fixed budget and fixed seed on models from the
   bitwise-verified set, run twice: once on a BridgeStan-compiled
   model, once on the stanli pair. Same header code, same RNG stream,
   bitwise-identical gradients; the draws must match bitwise, with
   any mismatch treated as a bug until explained. nutpie gets the
   same harness shape if its draws prove run-to-run deterministic.
   The CI job needs a C++ toolchain for the reference compile; CI has
   one, users never do.

## Risks and open questions

- **ABI drift**: BridgeStan's ABI moves slowly but moves. Pin the
  version, expose it via the version globals (clients warn rather
  than fail on mismatch), and let the conformance test catch drift.
- **`bs_param_initialize` semantics**: verify against the reference
  implementation during the RED phase.
- **propto/jacobian flags**: served value ignores them (documented).
  Acceptable for the motivating samplers; a client wanting a
  normalized density will get lp__ instead.
- **Windows**: `dladdr` needs a `GetModuleHandleExW` analog and the
  clone step a CoW-less copy. Deferred; the facade compiles out on
  Windows in v1.
- **Copy cost**: on filesystems without CoW the pair costs a full
  library copy (about 22 MB) per model in the cache dir. Acceptable;
  the helper reuses an existing pair.
- **Thread-safety of wheels**: multi-threaded clients need
  STAN_THREADS wheels. Believed true of released wheels; verify and
  document.

## Phasing

1. Facade core: bridgestan_abi TU, ExecutorPool, section boundaries
   in lowering, C++ unit tests.
2. Python helper + dlopen integration + conformance tests.
3. nutpie shim, docs, sampler parity harness.
4. (Separate effort, after benchmarking) default-sampler decision and
   possible walnutpie vendoring.
