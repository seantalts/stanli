# Hacking on stanli

A map for contributors: which file owns what, how a gradient gets
computed, and the recipes for the most common changes.
 - [`docs/how-it-works.md`](how-it-works.md) explains why the design is
what it is
 - [`runtime/src/OPTIMIZATIONS.md`](../runtime/src/OPTIMIZATIONS.md)
explains the graph passes in plain language
 - [`docs/benchmarks.md`](benchmarks.md) has the measurements

## Layout

| Path | Owns |
|---|---|
| [`runtime/include/stanli/`](../runtime/include/stanli/) | Public headers. [`graph.hpp`](../runtime/include/stanli/graph.hpp) is the IR: `Slot` + `Op` over flat arenas. |
| [`runtime/src/lower.cpp`](../runtime/src/lower.cpp) | The compiler: transformed MIR in, op graph out. |
| [`runtime/src/mir_reader.cpp`](../runtime/src/mir_reader.cpp) | Parses stanc3's s-expressions into MIR structs. Unrecognized input fails loudly. |
| [`runtime/src/inplace.cpp`](../runtime/src/inplace.cpp), [`constfold.cpp`](../runtime/src/constfold.cpp), [`reroll.cpp`](../runtime/src/reroll.cpp), [`island.cpp`](../runtime/src/island.cpp) | The graph passes, in pipeline order, each with an off switch (see [OPTIMIZATIONS.md](../runtime/src/OPTIMIZATIONS.md)). |
| [`runtime/src/executor.cpp`](../runtime/src/executor.cpp) | Runs the op list. `STANLI_PROFILE=1` prints per-opcode accounting. |
| [`runtime/kernels/`](../runtime/kernels/) | Op implementations: [`densities.cpp`](../runtime/kernels/densities.cpp), [`elementwise.cpp`](../runtime/kernels/elementwise.cpp), [`constrain.cpp`](../runtime/kernels/constrain.cpp), plus [`matrix_fns.cpp`](../runtime/kernels/matrix_fns.cpp)/[`legacy_fns.cpp`](../runtime/kernels/legacy_fns.cpp) wrapping stan-math functions without a native port ([`legacy.hpp`](../runtime/include/stanli/legacy.hpp) is the mechanism). |
| [`runtime/include/stanli/mir_interp.hpp`](../runtime/include/stanli/mir_interp.hpp) | The MIR interpreter, templated on the scalar: transformed data at lowering time, uncompiled ODE right-hand sides, interpreted write_array. |
| [`runtime/src/wa_interp.cpp`](../runtime/src/wa_interp.cpp) | Per-draw interpreted generated quantities when the write_array graph cannot be built (RNG calls, draw-dependent branches). |
| [`runtime/include/stanli/program.hpp`](../runtime/include/stanli/program.hpp), [`mir_prog.hpp`](../runtime/include/stanli/mir_prog.hpp) | The register machine and its MIR front end. |
| [`runtime/src/nuts.cpp`](../runtime/src/nuts.cpp) | The sampler: stan's own `adapt_diag_e_nuts`. Owns CmdStan parity of the RNG stream and initial-point acceptance. |
| [`runtime/src/capi.cpp`](../runtime/src/capi.cpp), [`capi.h`](../runtime/include/stanli/capi.h) | The C ABI. [`python/stanli/__init__.py`](../python/stanli/__init__.py) is a thin ctypes wrapper over it. |
| [`runtime/src/stanc_embed_c.cpp`](../runtime/src/stanc_embed_c.cpp), [`tools/stanc_embed/`](../tools/stanc_embed/) | The in-process stanc3 (OCaml, `-output-complete-obj`). |
| [`js/`](../js/), [`web/`](../web/) | The npm package (a one-call `sample()` over a worker) and the demo page, assembled from it by [`tools/build_web.sh`](../tools/build_web.sh) so they cannot drift. |
| [`tools/`](../tools/) | [`stanli_check`](../tools/stanli_check.cpp) (one deterministic gradient), [`stanli_run`](../tools/stanli_run.cpp) (full CSV run), [`dump_ops`](../tools/dump_ops.cpp) (print a model's op list), [`verify_refs.py`](../tools/verify_refs.py) (corpus replay against recorded CmdStan values; runs in CI), [`verify_sample.py`](../tools/verify_sample.py) (records the references; needs CmdStan), [`sampler_trace.py`](../tools/sampler_trace.py), [`gen_docs.py`](../tools/gen_docs.py), [`wasm_check.sh`](../tools/wasm_check.sh). |
| [`harnesses/`](../harnesses/) | Corpus sweeps needing a local posteriordb: [`wa_coverage.py`](../harnesses/wa_coverage.py), [`ab_corpus.py`](../harnesses/ab_corpus.py), benchmarks. |
| [`tests/`](../tests/) | One `test_*.cpp` per subsystem, plus [`fixtures/`](../tests/fixtures/) with `.stan` sources and pinned MIR (regenerate with [`tools/gen_fixtures.sh`](../tools/gen_fixtures.sh)). |

About 12k ish lines of runtime against 6k lines of tests. The biggest
files: [`lower.cpp`](../runtime/src/lower.cpp) (2k lines, the
compiler, most likely what you are looking for),
[`mir_interp.hpp`](../runtime/include/stanli/mir_interp.hpp) (1.5k),
[`reroll.cpp`](../runtime/src/reroll.cpp) (<1k). The kernels are
repetitive by design: read one and you can read them all. There are 82
opcodes ([`optable.hpp`](../runtime/include/stanli/optable.hpp)).

## Life of a gradient

```
model.stan + data.json
  |  stanc3, linked in-process (stanc_embed)
  v  transformed MIR, as s-expressions
mir_reader.cpp
  |  parsed into mir::Stmt / mir::Expr
  v
lower.cpp
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
  |  slot offsets assigned, arenas sized, one KernelCtx built per op,
  |  dispatch tables resolved
  v
forward sweep   = log density, each kernel stashing its partials
reverse sweep   = gradient, each kernel contracting them
```

**The graph is the autodiff tape**: no tape
is built at evaluation time, which is why parameter-dependent control
flow cannot be ops (it becomes an island) and why a steady-state
gradient allocates nothing. 

## The IR

Four types in [`graph.hpp`](../runtime/include/stanli/graph.hpp),
deliberately simple:

- `Slot`: a value. An arena offset, a length, and whether it is a
  parameter. Parameters come first, in declaration order, so the
  gradient vector is contiguous.
- `Op`: an opcode, up to six input slots, one output (rarely two),
  integer immediates (`idata`), an opaque payload (`udata`), and a
  scratch window.
- `Graph`: the slots, the ops, and the pools that own `idata`/`udata`.
- `KernelCtx`: what a kernel sees. Raw `double*` + length per input,
  output, and adjoint, plus scratch. Assembled once at bind.

The `variant` byte on `Op` encodes the CmdStan semantics a density must
reproduce: bits 0-5 are per-argument activity (which arguments are
autodiff), bit 6 is elementwise output, bit 7 is propto. Get it wrong
and the gradient stays perfect while the log density is off by a
constant.

## The kernel contract

A kernel is three function pointers
([`optable.hpp`](../runtime/include/stanli/optable.hpp)):

```cpp
struct Kernel {
  void (*forward)(KernelCtx&);            // writes ctx.out, stashes partials
  void (*backward)(KernelCtx&);           // accumulates into ctx.in_adj[k]
  int64_t (*scratch_size)(const Op&, const Slot*);   // doubles needed
};
```

Some rules:
- **`in_adj[k].data` may be null.** The input is data; do not touch its
  adjoint.
- **Accumulate in the order the reference does.** Several kernels sum
  in a deliberate direction to stay bitwise identical to the stan-math
  var path they are checked against. A reordered reduction usually
  still passes the corpus oracle at 1e-9 and still breaks
  [`test_matvec`](../tests/test_matvec.cpp) at 1 ULP, and the ULP is
  the thing worth keeping.
- **Scratch is yours alone**, sized at bind and never touched by
  another op.

## The register machine

Some code cannot be lowered into ops within the main fixed size autodiff region.
An ODE right-hand side must stay callable
because the integrator picks the times; a region whose control flow
depends on a parameter must pick its arm at evaluation time. Both
compile to the same flat instruction list over a register file
([`program.hpp`](../runtime/include/stanli/program.hpp)), run by one
`run_program<T>`: `double` for values, `var` where stan-math's autodiff
needs to see the arithmetic. Three front ends produce one: `compile_rhs`
([`ode_prog.cpp`](../runtime/src/ode_prog.cpp)) for ODE right-hand
sides, `carve_islands` ([`island.cpp`](../runtime/src/island.cpp)) for
runs of graph ops, and `lower_param_ifelse`/`lower_param_ternary`
([`lower.cpp`](../runtime/src/lower.cpp)) for parameter-conditional
statements. The first two are optimizations and can be declined
(`STANLI_NO_ISLAND=1` turns the pass off; `STANLI_ISLAND_ALWAYS=1`
skips the cost estimate). The third is not: without it the model does
not compile.

## The sampler

[`nuts.cpp`](../runtime/src/nuts.cpp) is 133 lines because Stan's own
classes do the sampling; the file's real job is matching CmdStan's
*configuration*, which no pointwise gradient test can see. 
[`tools/sampler_trace.py`](../tools/sampler_trace.py) is the oracle:
same model, same seed, compare the sampler columns distributionally.

## Where the bugs have lived

- **A destructive write in front of a backward that re-reads its
  inputs.** The in-place pass may only write over a value when every
  earlier op's backward is pure adjoint routing
  (`backward_ignores_input_values`, pinned by
  [`test_pass_safety.cpp`](../tests/test_pass_safety.cpp)). Getting
  this wrong made eight models wrong by up to 1.7e+05 relative with
  their op counts unchanged.
- **Propto and argument types.** Which constants a density drops
  depends on which arguments are autodiff. The register machine binds
  arguments uniformly and cannot reproduce that, which is why `~`
  inside a parameter-conditional region is refused rather than
  approximated: the bug it prevents had a perfect gradient and an lp
  off by exactly log(2*pi)/2.
- **Aliasing a slot that a region both reads and writes.** An island's
  live-in that is also a live-out needs a fresh slot, or the producer's
  adjoint is counted twice (measured: off by exactly 1.0).
- **Reassociation.** See the kernel contract. Cheap to do by accident,
  invisible in the corpus oracle, visible in the ULP tests.
- **Modes that skip work.** `forward_value_only()` lets a kernel skip
  partials; it is safe only because `gradient()` always runs a full
  forward first. If you add a mode like that, document why the skipped
  work cannot survive into a reverse sweep.

The practice that catches these is mutation testing, and it is expected
of any change that claims a safety property: break the guard on
purpose, watch the test fail, put it back. If the test passes with the
guard removed, the test is not testing the guard. (Delete the object
file and `touch` the source first; make's mtime granularity will hand
you a stale binary and a green run.)

## Adding a stan-math function

Decide where it runs. Anything the log density touches needs to be an
op. Functions that appear only in transformed data, ODE right-hand
sides, or generated quantities need only the interpreter: add a branch
in [`mir_interp.hpp`](../runtime/include/stanli/mir_interp.hpp)'s
`eval_fun`, or for RNG draws in
[`wa_interp.cpp`](../runtime/src/wa_interp.cpp)'s `rng_fun`. Both fail
loudly on anything unhandled;
`python3 harnesses/wa_coverage.py deps/posteriordb` shows what is
missing.

A density, distribution function, or scalar math function is one line
in an X-macro list in
[`optable.hpp`](../runtime/include/stanli/optable.hpp)
(`STANLI_SCALAR_DENSITY_LIST` and friends), which generates the opcode,
kernel, registration and lowering entry together so they cannot drift.
Then check it: `harnesses/fn_sweep.py deps/cmdstan --filter yourfn`
generates a one-function model, compiles it with CmdStan, and reports
the worst ULP.

Not everything fits: the recorder computes in doubles, so a stan-math
function that does arithmetic on the scalar type (`von_mises_cdf`,
`wiener_lpdf`) fails to compile rather than silently losing partials.
[`docs/coverage.md`](coverage.md) lists which and why.

Anything else takes four steps:

1. Opcode in [`optable.hpp`](../runtime/include/stanli/optable.hpp),
   kernel in [`runtime/kernels/`](../runtime/kernels/). For a stan-math
   function without a native port, wrap it with
   [`legacy.hpp`](../runtime/include/stanli/legacy.hpp) (examples in
   [`matrix_fns.cpp`](../runtime/kernels/matrix_fns.cpp)); it is
   correct by construction and can be replaced by a native kernel when
   it shows up in profiles.
2. Lowering branch in the matching `lower_*_fn` group in
   [`lower.cpp`](../runtime/src/lower.cpp).
3. A test in the matching `tests/test_*.cpp`, asserting parity against
   the same stan-math call on `var` (house pattern in
   [`test_lower.cpp`](../tests/test_lower.cpp)).
4. `build-rel/dump_ops model.stan data.json` shows what actually
   lowered.

## Verifying a change

```
cmake --build build-rel -j8 && (cd build-rel && ctest)
python3 tools/verify_refs.py deps/posteriordb --check build-rel/stanli_check --jobs 8
```

The corpus replay runs all 119 models against recorded CmdStan values
and is the strongest oracle in the project. It also runs in CI on every
push, and [`tools/wasm_check.sh`](../tools/wasm_check.sh) drives the
same replay through the WASM build. Compiler changes that claim to be
pure refactors should leave its worst-deviation line untouched.

If you touched a density tier or anything about propto, also check the
lite build (CI does not, because it would mean a second full stan-math
compile):

```
cmake -B build-lite -DCMAKE_BUILD_TYPE=Release -DSTANLI_LITE_LP=ON
cmake --build build-lite --target stanli_check -j8
python3 tools/verify_lite.py deps/posteriordb
python3 tools/verify_refs.py deps/posteriordb --check build-lite/stanli_check --no-lp
```

For sampler changes use
[`tools/sampler_trace.py`](../tools/sampler_trace.py); for
generated-quantities coverage,
[`harnesses/wa_coverage.py`](../harnesses/wa_coverage.py); for
performance claims, measure with `STANLI_PROFILE=1` before and after
([`docs/benchmarks.md`](benchmarks.md) has the harnesses).

## Landing a change

`main` is protected.

```
git checkout -b my-change
git push -u origin my-change
gh pr create --fill
gh pr merge --auto --rebase
```

The merge happens when CI goes green. Release tags (`v*`, `npm-v*`) are
not gated by this; they point at commits that already passed on main.

If a change plausibly touches Windows (build files, the C ABI surface,
`tools/exported_symbols.def`), run the job on the branch first:

```
gh workflow run wheels.yml --ref my-change
```

Release process and CI layout: [`README.md`](../README.md) under
"Releasing".
