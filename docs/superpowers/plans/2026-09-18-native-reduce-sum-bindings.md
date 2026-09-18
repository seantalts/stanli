# Native reduce_sum bindings integration

Base: fetched origin/HEAD (origin/main) at
`5931b66623438685cc11c64d2968b7a6c96fb237`. Existing native implementation
preserved; no integration conflicts. Stan 2.40 dependencies and embedded
frontend are the same verified inputs as the native integration record.

## Contract

Expose `threads_per_chain=1` on Python `Model.sample`, R `sample_model`,
`sample_cstan`, and `cstan_model(...)$sample`. Both model constructors also
accept the option for direct gradients. Sampling explicitly defaults to one.
Changing this setting prepares a new graph. Seed-dependent transformed data
is rebuilt under the run seed at the same time. Python adopts the new model
only after successful construction; R keeps value semantics and stores the
prepared model in the returned fit.

Additive C constructors preserve ABI version 1 and all existing option struct
layouts. A handle owns its persistent team. Multi-chain sampling builds teams
for executor clones once per call, with destruction ordering that keeps teams
alive until after their borrowers die. Graphs without retained reductions
create no worker team. Direct gradient calls reuse the model's team.

Graph-level retained counts and lowering refusal strings are exposed in C,
Python, and R. These do not inventory opaque runtime regions. Unsupported or
small callbacks remain serial; values above one require a thread-safe build.
Bindings validate positive integer counts and C-int overflow. Older runtimes
remain usable for serial calls; requesting the new capability names the
missing runtime support instead of silently running serially.

## Evaluator and results

Use a normal likelihood reduction over 10,000 observations, a shared active
mean, a proper normal prior, and generated-quantity RNG output. Compare
parallel versus serial gradients (relative tolerance 2e-12), an independent
analytic gradient, and bitwise draws for fixed partitions under sequential
versus concurrent chain scheduling. Also test switching back to the serial
graph, source and MIR construction, static reductions, small-input refusal,
invalid counts, missing runtime entrypoints, and transformed-data seed
forwarding. The C test verifies that a direct gradient remains valid after
sampling has destroyed its cloned executors and teams.

Validation on this base:

- Release `build-reduce-sum-native240`, STANLI_THREADS=ON, embedded stanc:
  complete CTest suite **259/259 passed**. Log: `bindings-ctest.log`.
- Python bundled runtime interpreter with NumPy, `PYTHONPATH=python`:
  `tests/test_python.py` **50/50 passed**. Log: `python-bindings.log`.
- R 4.6.1, package installed with `R CMD INSTALL --preclean` into the build's
  private `r-library`; complete `testthat::test_dir("r/tests/testthat",
  package="stanli")` passed with no skips. STANLI_RUNTIME pointed to this
  build's library and STANLI_RUN to its CLI. Log: `r-bindings.log`.
- ThreadSanitizer `build-reduce-sum-native240-tsan`: `test_capi` passed,
  including the new MIR-based retained reduction, concurrent chains,
  repeated sampling, and post-sampling gradient. Log: `capi-test.log`.
- R documentation regenerated with roxygen2; `git diff --check` passed.

Two previous R expectations were updated because they asserted that the
option was discarded or unsupported. New end-to-end tests require the native
reduction count to be one, so merely accepting the keyword cannot pass.
No new performance claim is made for the bindings; the native kernel timing
and memory evidence remain in `docs/native-reduce-sum-integration.json`.
