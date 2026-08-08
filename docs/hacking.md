# Hacking on stanli

A map for contributors: which file owns what, how a gradient actually
gets computed, and the recipes for the two most common changes.
[`docs/how-it-works.md`](how-it-works.md) explains why the design is what it is;
[`runtime/src/OPTIMIZATIONS.md`](../runtime/src/OPTIMIZATIONS.md) explains the graph passes in plain
language; [`docs/benchmarks.md`](benchmarks.md) has the measurements.

If you are reading only one section, read "Where the silent wrongness
lives" -- it is the shape of every bug this project has shipped.

## Layout

| Path | Owns |
|---|---|
| [`runtime/include/stanli/`](../runtime/include/stanli/) | All public headers. [`graph.hpp`](../runtime/include/stanli/graph.hpp) is the IR: `Slot` + `Op` over flat arenas. |
| [`runtime/src/lower.cpp`](../runtime/src/lower.cpp) | The compiler: transformed MIR in, op graph out. `lower_expr`/`lower_stmt` walk statements; function calls dispatch through `lower_density_fn`, `lower_eltwise_fn`, `lower_matrix_fn`, `lower_ode_fn`. `lower_read_param` builds the constrain ops and the parameter views. |
| [`runtime/src/mir_reader.cpp`](../runtime/src/mir_reader.cpp), [`sexp.hpp`](../runtime/include/stanli/sexp.hpp), [`mir.hpp`](../runtime/include/stanli/mir.hpp) | Parse stanc3's `--debug-transformed-mir` s-expressions into the C++ MIR structs. Anything unrecognized is preserved as raw text and fails loudly if reached. |
| [`runtime/src/inplace.cpp`](../runtime/src/inplace.cpp), [`constfold.cpp`](../runtime/src/constfold.cpp), [`reroll.cpp`](../runtime/src/reroll.cpp) | The graph passes, in pipeline order (with [`island.cpp`](../runtime/src/island.cpp), below, closing it). Each has an env switch to turn it off (see [OPTIMIZATIONS.md](../runtime/src/OPTIMIZATIONS.md)). |
| [`runtime/src/executor.cpp`](../runtime/src/executor.cpp) | Runs the op list: forward for the log density, reverse for the gradient. `STANLI_PROFILE=1` prints per-opcode accounting. |
| [`runtime/kernels/`](../runtime/kernels/) | Op implementations. [`densities.cpp`](../runtime/kernels/densities.cpp) instantiates unmodified stan-math prim templates; [`elementwise.cpp`](../runtime/kernels/elementwise.cpp)/[`eltwise_expr.cpp`](../runtime/kernels/eltwise_expr.cpp) the vector math; [`constrain.cpp`](../runtime/kernels/constrain.cpp) the transforms; [`matrix_fns.cpp`](../runtime/kernels/matrix_fns.cpp) and [`legacy_fns.cpp`](../runtime/kernels/legacy_fns.cpp) wrap stan-math functions that have no native port (see [`legacy.hpp`](../runtime/include/stanli/legacy.hpp) for the mechanism). |
| [`runtime/include/stanli/mir_interp.hpp`](../runtime/include/stanli/mir_interp.hpp) | The one MIR interpreter, templated on the scalar. Three users: the lowering (transformed data and every data-only expression, on `double`), the ODE kernels (right-hand sides the compiled path cannot handle, on `double` and `var`), and the interpreted write_array. |
| [`runtime/src/wa_interp.cpp`](../runtime/src/wa_interp.cpp) | `WaInterp`: per-draw interpreted generated quantities for models whose write_array graph cannot be built (RNG calls, draw-dependent branches). Owns the RNG stream and the FnReadParam/FnWriteParam statement hooks. |
| [`runtime/src/island.cpp`](../runtime/src/island.cpp) | The tape-island carver and its cost estimate, plus the graph-op front end to the register machine. Runs last in the pipeline. |
| [`runtime/include/stanli/program.hpp`](../runtime/include/stanli/program.hpp), [`mir_prog.hpp`](../runtime/include/stanli/mir_prog.hpp) | The register machine: one instruction set with one runner templated on the scalar ([`program.hpp`](../runtime/include/stanli/program.hpp)), and the MIR front end both callers compile through ([`mir_prog.hpp`](../runtime/include/stanli/mir_prog.hpp)). [`ode_prog.cpp`](../runtime/src/ode_prog.cpp) is the ODE entry, [`island.cpp`](../runtime/src/island.cpp) the graph-op entry, [`lower.cpp`](../runtime/src/lower.cpp) the parameter-conditional entry. |
| [`runtime/src/nuts.cpp`](../runtime/src/nuts.cpp) | The sampler: stan's own `adapt_diag_e_nuts` driven through [`model_adapter.hpp`](../runtime/include/stanli/model_adapter.hpp). Also owns CmdStan parity of the RNG stream and of which initial points are accepted. |
| [`runtime/src/capi.cpp`](../runtime/src/capi.cpp), [`capi.h`](../runtime/include/stanli/capi.h) | The C ABI the shared library exports, write_array included. [`python/stanli/__init__.py`](../python/stanli/__init__.py) is a thin ctypes wrapper over it. |
| [`runtime/src/stanc_embed_c.cpp`](../runtime/src/stanc_embed_c.cpp), [`tools/stanc_embed/`](../tools/stanc_embed/) | The in-process stanc3: the OCaml compiler built with `-output-complete-obj` and linked into the shared library. |
| [`js/`](../js/) | The npm package: [`index.mjs`](../js/index.mjs) is the one-call `sample()` API over [`worker.js`](../js/worker.js), which runs stancjs (stanc3 as JavaScript) and the WASM build of this runtime off the main thread. |
| [`web/`](../web/) | The demo page at [seantalts.github.io/stanli](https://seantalts.github.io/stanli/), assembled from [`js/`](../js/) by [`tools/build_web.sh`](../tools/build_web.sh) so the package and the page cannot drift. |
| [`tools/`](../tools/) | [`stanli_check`](../tools/stanli_check.cpp) (one deterministic gradient evaluation, machine-readable), [`stanli_run`](../tools/stanli_run.cpp) (full CSV sampling run), [`dump_ops`](../tools/dump_ops.cpp) (print a model's lowered op list), [`verify_refs.py`](../tools/verify_refs.py) (corpus replay against recorded CmdStan values, runs in CI), [`verify_sample.py`](../tools/verify_sample.py) (records those references, needs CmdStan), [`sampler_trace.py`](../tools/sampler_trace.py) (sampler-column diff vs CmdStan), [`gen_docs.py`](../tools/gen_docs.py) (stamps measured numbers into the READMEs), [`wasm_check.sh`](../tools/wasm_check.sh) (the corpus replay through the WASM build, under Node). |
| [`harnesses/`](../harnesses/) | Corpus sweeps that need a local posteriordb: [`wa_coverage.py`](../harnesses/wa_coverage.py) (how much of each model's generated quantities we produce), [`wa_header_check.py`](../harnesses/wa_header_check.py) (CSV headers vs CmdStan), benchmarks. |
| [`tests/`](../tests/) | One `test_*.cpp` per subsystem, plus [`fixtures/`](../tests/fixtures/) with `.stan` sources and their pinned `.tmir.sexp` MIR (regenerate with [`tools/gen_fixtures.sh`](../tools/gen_fixtures.sh)). |

## The shape of the thing

About 11,500 lines of runtime against 5,800 lines of tests. Where the
weight sits:

| | lines | |
|---|---:|---|
| [`lower.cpp`](../runtime/src/lower.cpp) | 1,935 | The compiler. Biggest file, and the one most likely to be what you are looking for. |
| [`mir_interp.hpp`](../runtime/include/stanli/mir_interp.hpp) | 1,284 | The MIR interpreter, templated on the scalar. |
| [`reroll.cpp`](../runtime/src/reroll.cpp) | 860 | The vectorizing pass. Dense, but self-contained. |
| [`mir_prog.hpp`](../runtime/include/stanli/mir_prog.hpp) | 536 | MIR to register program. |
| [`island.cpp`](../runtime/src/island.cpp) | 528 | The island carver. |
| [`matrix_fns.cpp`](../runtime/kernels/matrix_fns.cpp), [`densities.cpp`](../runtime/kernels/densities.cpp) | 488, 455 | Kernels. Repetitive by design; read one, you can read them all. |
| [`mir_reader.cpp`](../runtime/src/mir_reader.cpp) | 374 | S-expressions to MIR structs. |
| [`executor.cpp`](../runtime/src/executor.cpp) | 320 | Runs the op list. Small on purpose. |
| [`inplace.cpp`](../runtime/src/inplace.cpp), [`constfold.cpp`](../runtime/src/constfold.cpp) | 204, 186 | The other two passes. |
| [`nuts.cpp`](../runtime/src/nuts.cpp) | 133 | The sampler is thin: Stan's classes do the work. |

There are 82 opcodes ([`optable.hpp`](../runtime/include/stanli/optable.hpp)). Adding one is a small, well-worn
change; the recipe is below. The difficulty in this codebase is almost
never in writing new code -- it is in not breaking the numerical
agreement with CmdStan that everything else rests on, which is what the
"silent wrongness" section is about.

## Life of a gradient

```
model.stan + data.json
  |  stanc3, linked in-process (stanc_embed)
  v  transformed MIR, as s-expressions
mir_reader.cpp
  |  parsed into mir::Stmt / mir::Expr
  v
lower.cpp                       <- the compiler; ~everything hard lives here
  |  transformed data evaluated eagerly (mir_interp.hpp, on double)
  |  data-bound loops unrolled, `~` lowered with CmdStan's propto and
  |  per-argument activity, parameter-conditional regions compiled to
  |  islands (mir_prog.hpp)
  v  Graph: Slot[] + Op[]
graph passes, in this order (lower.cpp's run()):
  |  make_inplace_updates      copy-then-modify -> in-place write
  |  forward_stores_to_loads   write x[n], read x[n] -> use the value
  |  const_fold                ops no parameter can reach -> constants
  |  reroll                    N copies of a template -> vector ops
  |  carve_islands             scalar residue -> one register program
  |  reduce_terms              target terms -> one result slot
  v
Executor::bind_()
  |  slot offsets assigned, three arenas sized (values, adjoints,
  |  scratch), one KernelCtx built per op, dispatch tables resolved
  v
forward sweep   = log density, each kernel stashing its partials
reverse sweep   = gradient, each kernel contracting them
```

Two things follow from that picture and are worth internalizing.

**The graph is the autodiff tape.** There is no tape being built at
evaluation time; the op list *is* the tape, fixed when the model loads.
That is why control flow that depends on a parameter cannot be ops (it
becomes an island instead), and why a steady-state gradient allocates
nothing.

**Lowering happens once, evaluation happens millions of times.** A
compile-time cost of 200 ms to save 10 ns per gradient is a trade this
project takes every time.

## The IR

Four types in [`graph.hpp`](../runtime/include/stanli/graph.hpp), and they are deliberately dull:

- `Slot` -- a value: an offset into the arenas, a length, and whether it
  is a parameter. Parameters come first, in declaration order, so the
  gradient vector is contiguous.
- `Op` -- an opcode, up to six input slots, one output (rarely two),
  integer immediates (`idata`), an opaque payload (`udata`, for ODE
  specs and island programs), and a scratch window.
- `Graph` -- the slots, the ops, and the pools that own `idata`/`udata`.
- `KernelCtx` -- what a kernel actually sees: raw `double*` + length for
  each input, output, and adjoint, plus its scratch. Assembled once at
  bind, never rebuilt.

The `variant` byte on `Op` is worth reading twice, because it encodes
the CmdStan semantics a density has to reproduce: bits 0-5 are
per-argument activity (which arguments are autodiff), bit 6 means the
output is elementwise (one lp per element rather than the sum), and bit
7 is propto. Get it wrong and the gradient stays perfect while the log
density is off by a constant.

## The kernel contract

A kernel is three function pointers ([`optable.hpp`](../runtime/include/stanli/optable.hpp)):

```cpp
struct Kernel {
  void (*forward)(KernelCtx&);            // writes ctx.out, stashes partials
  void (*backward)(KernelCtx&);           // accumulates into ctx.in_adj[k]
  int64_t (*scratch_size)(const Op&, const Slot*);   // doubles needed
};
```

The rules, all of which have been learned the hard way:

- **The forward computes partials; the backward only contracts them.**
  Not the other way around. A backward that recomputes the forward pays
  twice per gradient -- that was 90% of `diamonds`, and fixing it was
  worth 1.8x on the model.
- **`in_adj[k].data` may be null.** That means the input is data and its
  adjoint must not be touched.
- **Accumulate in the order the reference does.** Several kernels sum in
  a deliberate direction (descending, or per-element rather than
  blocked) to stay bitwise identical to the stan-math var path they are
  checked against. If you reorder a reduction you will usually still
  pass the corpus oracle at 1e-9 and still break [`test_matvec`](../tests/test_matvec.cpp) at 1 ULP,
  and the ULP is the thing worth keeping.
- **Scratch is yours alone**, sized at bind and never touched by another
  op -- which is what makes an in-place write safe in front of you.

## The register machine

Some code cannot be ops. An ODE right-hand side has to stay callable
because the integrator picks the times; a region whose control flow
depends on a parameter has to pick its arm at evaluation time. Both
compile to the same flat instruction list over a register file
([`program.hpp`](../runtime/include/stanli/program.hpp)), run by one `run_program<T>` templated on the scalar --
`double` for values, `var` where stan-math's autodiff needs to see the
arithmetic.

Three front ends produce one:

| Entry | Source | Who runs it |
|---|---|---|
| `compile_rhs` ([`ode_prog.cpp`](../runtime/src/ode_prog.cpp)) | a MIR function body | the ODE kernel, per integrator step |
| `carve_islands` ([`island.cpp`](../runtime/src/island.cpp)) | a run of graph ops | `OP_ISLAND`, once per evaluation |
| `lower_param_ifelse` / `lower_param_ternary` ([`lower.cpp`](../runtime/src/lower.cpp)) | a MIR statement or expression | `OP_ISLAND`, once per evaluation |

The first two are optimizations and can be declined -- the carver keeps
an island only when it estimates the ops cost more (see
[OPTIMIZATIONS.md](../runtime/src/OPTIMIZATIONS.md);
`STANLI_NO_ISLAND=1` turns the pass off, `STANLI_ISLAND_ALWAYS=1` skips
the estimate when you want to know why a region was left alone). The
third is not: without it the model does not compile, so no switch
reaches it.

## The sampler

[`nuts.cpp`](../runtime/src/nuts.cpp) is 133 lines because Stan's own `adapt_diag_e_nuts` does the
sampling; the file's real job is matching CmdStan's *configuration*,
which is invisible to any pointwise gradient test. Three things it has
to keep in step, each of which was wrong at some point:

- The max tree depth, which defaults to 5 in the base class and 10 in
  CmdStan.
- The RNG stream: `stan::services::util::create_rng(seed, chain)`, the
  same engine seeded the same way, drawing the initial point in the same
  order. Different engines mean the same seed names different starting
  points, and every sampler comparison silently compares two draws.
- Which initial points are accepted: CmdStan evaluates the log density
  on doubles *and then* its gradient, rejecting on either. Those two can
  disagree for an ODE model, so `Executor::forward_value_only()` exists
  to be the double path.

[`tools/sampler_trace.py`](../tools/sampler_trace.py) is the oracle for all of this: same model, same
seed, compare the sampler columns distributionally.

## Where the silent wrongness lives

Every bug this project has shipped has been of one kind: the graph
changed shape, every test passed, and the numbers were wrong. The
classes, with the case that taught each:

- **A destructive write in front of a backward that re-reads its
  inputs.** The in-place pass may only write over a value when every
  earlier op's backward is pure adjoint routing
  (`backward_ignores_input_values`, pinned by [`test_pass_safety.cpp`](../tests/test_pass_safety.cpp)).
  Getting this wrong made eight models wrong by up to 1.7e+05 relative
  with their op counts unchanged.
- **Propto and argument types.** Which constants a density drops depends
  on which arguments are autodiff. Anything that binds arguments
  uniformly (the register machine) cannot reproduce it, which is why
  `~` inside a parameter-conditional region is refused rather than
  approximated -- it had a perfect gradient and an lp off by exactly
  log(2*pi)/2.
- **Aliasing a slot that is both read and written by the same region.**
  An island's live-in that is also a live-out needs a fresh slot, or the
  producer's adjoint is counted twice (measured: off by exactly 1.0).
- **Reassociation.** See the kernel contract. Cheap to do by accident,
  invisible in the corpus oracle, visible in the ULP tests.
- **Modes that skip work.** `forward_value_only()` lets a kernel skip
  partials; it is safe only because `gradient()` always runs a full
  forward first. If you add a mode like that, say out loud why the
  skipped work cannot survive into a reverse sweep.

The practice that catches these is mutation testing, and it is expected
of a change that claims a safety property: break the thing on purpose,
watch the test fail, put it back. If the test passes with the guard
removed, the test is not testing the guard. (Delete the object file and
`touch` the source first -- make's mtime granularity will happily hand
you a stale binary and a green run.)

## Adding a stan-math function

Decide where it runs. Anything the log density touches needs to be an op;
functions that only appear in transformed data, ODE right-hand sides, or
generated quantities only need the interpreter.

Interpreter vocabulary: add a branch in [`mir_interp.hpp`](../runtime/include/stanli/mir_interp.hpp)'s `eval_fun`
(deterministic math, templated on the scalar) or, for RNG draws, in
[`wa_interp.cpp`](../runtime/src/wa_interp.cpp)'s `rng_fun`. Both fail loudly on anything unhandled, so
the corpus tells you what is missing:
`python3 harnesses/wa_coverage.py deps/posteriordb`.

A density, a distribution function or a scalar math function is one line
in an X-macro list in [`optable.hpp`](../runtime/include/stanli/optable.hpp)
-- `STANLI_SCALAR_DENSITY_LIST`, `STANLI_INT_DENSITY_LIST`,
`STANLI_SCALAR_CDF_LIST`, `STANLI_SCALAR_UNARY_LIST` -- which generates the
opcode, the name, the kernel, its registration and the lowering entry
together, so none of them can drift out of step. Then check it:
`harnesses/fn_sweep.py deps/cmdstan --filter yourfn`, which generates a
one-function model, compiles it with CmdStan, and reports the worst ULP.
The bar is 0.

Not everything fits: the recorder computes in doubles and carries no tape,
so a stan-math function that does arithmetic on the scalar type
(`von_mises_cdf`, `wiener_lpdf`) fails to compile rather than silently
losing partials. [`docs/coverage.md`](coverage.md) lists which and why.

Anything else takes four steps:

1. Opcode in [`optable.hpp`](../runtime/include/stanli/optable.hpp), kernel in [`runtime/kernels/`](../runtime/kernels/). For a stan-math
   function without a native port, wrap it with the mechanism in
   [`legacy.hpp`](../runtime/include/stanli/legacy.hpp) (examples all over [`matrix_fns.cpp`](../runtime/kernels/matrix_fns.cpp)); it is correct by
   construction and can be replaced by a native kernel when it shows up
   in profiles.
2. Lowering branch in the matching `lower_*_fn` group in [`lower.cpp`](../runtime/src/lower.cpp).
3. A test in the matching `tests/test_*.cpp`, asserting parity against
   the same stan-math call on `var` (house pattern in [`test_lower.cpp`](../tests/test_lower.cpp)).
4. `build-rel/dump_ops model.stan data.json` shows what actually lowered.

## Verifying a change

```
cmake --build build-rel -j8 && (cd build-rel && ctest)
python3 tools/verify_refs.py deps/posteriordb --check build-rel/stanli_check --jobs 8
```

If you touched a density tier, `density_tier`, or anything about propto,
also check the lite build -- CI does not, because it would mean a second
full stan-math compile:

```
cmake -B build-lite -DCMAKE_BUILD_TYPE=Release -DSTANLI_LITE_LP=ON
cmake --build build-lite --target stanli_check -j8
python3 tools/verify_lite.py deps/posteriordb
python3 tools/verify_refs.py deps/posteriordb --check build-lite/stanli_check --no-lp
```

The first replays both builds and holds gradients to the bit while
requiring the lp shift to be the same at every evaluation point; the
second checks the lite build's gradients against CmdStan directly.
[`docs/lite-lp.md`](lite-lp.md) explains what the flag trades.

The corpus replay above runs all 119 models against recorded CmdStan
values and is the strongest oracle in the project; it also runs in CI on
every push, on each of the four wheel platforms it is gated on, and
[`tools/wasm_check.sh`](../tools/wasm_check.sh) drives the same replay
through the WASM build under Node. Compiler changes that claim to be pure
refactors should leave its worst-deviation line untouched. For sampler
changes use [`tools/sampler_trace.py`](../tools/sampler_trace.py); for generated-quantities coverage,
[`harnesses/wa_coverage.py`](../harnesses/wa_coverage.py); for performance claims, measure with
`STANLI_PROFILE=1` before and after ([`docs/benchmarks.md`](benchmarks.md) has the
harnesses and the current numbers).

## Landing a change

`main` is protected: the four wheel platforms that build at a reasonable
speed, plus the WASM build, must pass before anything merges, admins
included, so direct pushes are rejected. The flow is a branch and an
auto-merged PR:

```
git checkout -b my-change
git push -u origin my-change
gh pr create --fill
gh pr merge --auto --rebase
```

The merge happens on its own when CI goes green (rebase-merge keeps the
history linear). Release tags (`v*`, `npm-v*`) are not gated by this;
they point at commits that already passed on main.

Windows is the exception. mingw compiles the density kernels an order of
magnitude slower than any other platform, so that job would set the pace
for every merge on its own; it runs after the merge, nightly at 08:00
UTC, and on release tags instead of on pull requests. A red Windows run
on main is a bug to fix forward, and a tag will not publish until it
passes. If a change plausibly touches Windows (build files, the C ABI
surface, anything under `tools/exported_symbols.def`), run the job on the
branch before merging:

```
gh workflow run wheels.yml --ref my-change
```

Release process and CI layout live in [`README.md`](../README.md) under "Releasing".
