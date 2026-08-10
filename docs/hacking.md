# Hacking on stanli

A guide for contributors: how the pieces fit together, which file owns
what, and recipes for the most common changes. It assumes you know
Stan. It does not assume you have worked on a compiler or an
interpreter; the handful of terms of art are explained as they come up.

Three companion documents:

- [`docs/how-it-works.md`](how-it-works.md) explains the design and why
  an interpreter can outrun a compiled model. If you are here for the
  performance story rather than to change the code, start there.
- [`runtime/src/OPTIMIZATIONS.md`](../runtime/src/OPTIMIZATIONS.md)
  explains each graph optimization in plain language, with the
  measurements behind it.
- [`docs/benchmarks.md`](benchmarks.md) has the numbers.
- [`docs/lowering-walkthrough.md`](lowering-walkthrough.md) traces
  three small models through every layer described here: the normal
  vectorized path, a parameter branch, and a recurrence with its
  generated backward.

## The idea, and five words for it

stanli never generates machine code. Every operation a Stan model can
perform (`normal_lpdf`, a matrix product, `exp` over a vector) is
compiled once, into the stanli library itself. Loading a model produces
a description: a list saying which of those precompiled operations to
run, in what order, on which values. Evaluating the model means walking
the list. A program that executes another program stored as data is
called an interpreter, and that is what stanli is.

Five terms cover most of the vocabulary in this document and in the
code:

- An **op** is one entry in the list. It holds an **opcode** (a number
  saying which operation to perform) plus references to the values it
  reads and the value it writes.
- A **kernel** is the precompiled C++ function that does one opcode's
  actual work. Ops are data; kernels are code.
- A **slot** is one value in the model: a scalar, vector, or matrix.
  All values live in one large array of doubles allocated up front
  (the **arena**), and a slot is an offset and a length into it.
- **Lowering** is the translation from the Stan compiler's output into
  the op list. Compiler people call any translation from a richer form
  into a simpler one "lowering", and call the simpler form an **IR**
  (intermediate representation). The op graph is stanli's IR.
- A **pass** is a function that walks the op list and rewrites it into
  an equivalent but faster list. stanli's passes are the subject of
  [OPTIMIZATIONS.md](../runtime/src/OPTIMIZATIONS.md).

Gradients come from reverse-mode automatic differentiation, the same
technique CmdStan uses. Run the list forward and you have the log
density. Then walk it backward: each op is handed the derivative of the
log density with respect to its own output (called the output's
**adjoint**) and converts it, by the chain rule, into contributions to
its inputs' adjoints. When the walk reaches the top, the adjoints of
the parameter slots are the gradient. The record that reverse mode
walks backward is called the **tape**. CmdStan builds a fresh tape on
every gradient evaluation; in stanli the op list *is* the tape, built
once at load. That one fact drives much of the design, and it comes up
repeatedly below.

## Life of a gradient

```
model.stan + data.json
  |  stanc3, linked in-process (stanc_embed)
  v  transformed MIR, as s-expressions
mir_reader.cpp
  |  parsed into mir::Stmt / mir::Expr structs
  v
lower.cpp
  |  MIR statements emitted as ops
  v  Graph: Slot[] + Op[]
graph passes
  |  the list rewritten shorter (in-place updates, store-to-load
  |  forwarding, constant folding, loop re-rolling, islands)
  v
Executor::bind_()
  |  arena offsets assigned, kernels resolved, contexts built
  v
forward sweep   = log density
reverse sweep   = gradient
```

Step by step:

1. **stanc3 compiles the model.** The official Stan compiler (an OCaml
   program) is linked into the stanli library and runs in-process
   ([`tools/stanc_embed/`](../tools/stanc_embed/)). Your model is
   parsed, typechecked, and optimized by exactly the code CmdStan uses.
   But instead of asking it to generate C++, stanli asks it to print
   its internal representation of the program, called MIR, as
   s-expressions (parenthesized text, in the Lisp style).

2. **[`mir_reader.cpp`](../runtime/src/mir_reader.cpp) parses that
   text** into plain C++ structs (`mir::Stmt`, `mir::Expr`). Anything
   it does not recognize is a loud error, never a guess.

3. **[`lower.cpp`](../runtime/src/lower.cpp) turns MIR into the op
   graph.** Along the way: transformed data is evaluated once, at
   load, using the MIR interpreter described below; loops whose bounds
   are data are unrolled, one copy of the body per iteration (the
   re-rolling pass will undo this); `~` statements are lowered with
   CmdStan's dropped-constant rules (see "the variant byte" below);
   and any statement whose control flow depends on a parameter is
   compiled for the register machine (its own section below).

4. **The passes rewrite the list.** In pipeline order: in-place
   updates, store-to-load forwarding, constant folding, loop
   re-rolling, islands, and finally `reduce_terms`, which sums all the
   target contributions into one slot. Each pass is one file, one
   section of [OPTIMIZATIONS.md](../runtime/src/OPTIMIZATIONS.md), and
   one environment switch that turns it off.

5. **[`executor.cpp`](../runtime/src/executor.cpp) binds the graph.**
   `bind_()` assigns every slot its arena offset, allocates the
   arenas, resolves each opcode to its kernel's function pointers, and
   builds each op a `KernelCtx`: the small struct of raw pointers the
   kernel will read. All of this happens once. Afterwards a gradient
   evaluation looks nothing up and allocates nothing.

6. **Forward, then reverse.** The forward sweep walks the list; each
   kernel computes its output and may stash partial derivatives in its
   scratch space. The reverse sweep walks the list backward; each
   kernel folds its output's adjoint into its inputs' adjoints.
   Parameter slots come first in the arena, in declaration order, so
   the finished gradient is one contiguous vector.

To watch all of this happen to real models,
[lowering-walkthrough.md](lowering-walkthrough.md) traces three small
ones through every stage, op list and register programs included.

## Layout

| Path | Owns |
|---|---|
| [`runtime/include/stanli/`](../runtime/include/stanli/) | Public headers. [`graph.hpp`](../runtime/include/stanli/graph.hpp) defines the IR: `Slot` and `Op` over flat arenas. |
| [`runtime/src/lower.cpp`](../runtime/src/lower.cpp) | Lowering: transformed MIR in, op graph out. |
| [`runtime/src/mir_reader.cpp`](../runtime/src/mir_reader.cpp) | Parses stanc3's s-expressions into MIR structs. |
| [`runtime/src/inplace.cpp`](../runtime/src/inplace.cpp), [`constfold.cpp`](../runtime/src/constfold.cpp), [`reroll.cpp`](../runtime/src/reroll.cpp), [`island.cpp`](../runtime/src/island.cpp) | The graph passes, in pipeline order. |
| [`runtime/src/executor.cpp`](../runtime/src/executor.cpp) | Runs the op list. `STANLI_PROFILE=1` prints per-opcode time accounting. |
| [`runtime/kernels/`](../runtime/kernels/) | The kernels: [`densities.cpp`](../runtime/kernels/densities.cpp), [`elementwise.cpp`](../runtime/kernels/elementwise.cpp), [`constrain.cpp`](../runtime/kernels/constrain.cpp), and friends. [`matrix_fns.cpp`](../runtime/kernels/matrix_fns.cpp) and [`legacy_fns.cpp`](../runtime/kernels/legacy_fns.cpp) wrap stan-math functions that have no native kernel yet. |
| [`runtime/include/stanli/mir_interp.hpp`](../runtime/include/stanli/mir_interp.hpp) | The MIR interpreter: runs MIR directly, without lowering to ops. Used where the op graph cannot go: transformed data at load time, ODE right-hand sides, interpreted `write_array`. |
| [`runtime/src/wa_interp.cpp`](../runtime/src/wa_interp.cpp) | Interpreted per-draw generated quantities, for models whose generated quantities cannot become a graph (RNG calls, draw-dependent branches). |
| [`runtime/include/stanli/program.hpp`](../runtime/include/stanli/program.hpp), [`mir_prog.hpp`](../runtime/include/stanli/mir_prog.hpp) | The register machine and its MIR front end. Its `DENSITY` instruction covers the 27 scalar continuous densities through [`program_density.hpp`](../runtime/include/stanli/program_density.hpp), one table shared with the MIR interpreter; its `CALL` instruction runs a graph kernel over a range of registers. |
| [`runtime/src/adjoint.cpp`](../runtime/src/adjoint.cpp) | `gen_adjoint`: differentiates a register program into a second register program, so an island's backward is a second cheap pass rather than a replay under stan-math's `var`. Owns the checkpoint analysis (save a register before a later write destroys a value a derivative rule needs). `STANLI_NO_NATIVE_ADJ=1` restores the replay, which is the oracle it is tested against ([`test_adjoint.cpp`](../tests/test_adjoint.cpp)). |
| [`runtime/src/nuts.cpp`](../runtime/src/nuts.cpp) | The sampler: Stan's own `adapt_diag_e_nuts`, configured to match CmdStan exactly. |
| [`runtime/src/capi.cpp`](../runtime/src/capi.cpp), [`capi.h`](../runtime/include/stanli/capi.h) | The C ABI. [`python/stanli/__init__.py`](../python/stanli/__init__.py) is a thin ctypes wrapper over it. |
| [`runtime/src/stanc_embed_c.cpp`](../runtime/src/stanc_embed_c.cpp), [`tools/stanc_embed/`](../tools/stanc_embed/) | The in-process stanc3. |
| [`js/`](../js/), [`web/`](../web/) | The npm package (a one-call `sample()` over a worker) and the demo page. [`tools/build_web.sh`](../tools/build_web.sh) builds the page from the package, so the two cannot drift. |
| [`tools/`](../tools/) | Small programs and scripts; the important ones are listed below. |
| [`harnesses/`](../harnesses/) | Corpus sweeps that need a local posteriordb: [`wa_coverage.py`](../harnesses/wa_coverage.py), [`ab_corpus.py`](../harnesses/ab_corpus.py), [`fn_sweep.py`](../harnesses/fn_sweep.py), benchmarks. |
| [`tests/`](../tests/) | One `test_*.cpp` per subsystem, plus [`fixtures/`](../tests/fixtures/) with `.stan` sources and pinned MIR (regenerate with [`tools/gen_fixtures.sh`](../tools/gen_fixtures.sh)). |

The tools you will reach for most:

- [`stanli_check`](../tools/stanli_check.cpp): one deterministic
  gradient evaluation, printed.
- [`stanli_run`](../tools/stanli_run.cpp): a full sampling run,
  CmdStan-style CSV out.
- [`dump_ops`](../tools/dump_ops.cpp): print the op list a model
  lowered to. Usually the first thing to look at.
- [`dump_islands`](../tools/dump_islands.cpp): print every island's
  forward and adjoint programs, instruction by instruction.
- [`verify_refs.py`](../tools/verify_refs.py): replay the whole model
  corpus against recorded CmdStan values. Runs in CI.
- [`verify_sample.py`](../tools/verify_sample.py): record those
  reference values (needs a CmdStan checkout).
- [`sampler_trace.py`](../tools/sampler_trace.py): compare sampler
  behavior against CmdStan.

For orientation: about 16,500 lines of runtime (not counting a
vendored JSON parser) and 10,000 lines of tests. The biggest files are
[`lower.cpp`](../runtime/src/lower.cpp) (2.4k lines, and the most
likely place your change belongs),
[`mir_interp.hpp`](../runtime/include/stanli/mir_interp.hpp) (1.3k),
and [`reroll.cpp`](../runtime/src/reroll.cpp) (0.9k). The opcode list
([`optable.hpp`](../runtime/include/stanli/optable.hpp)) has about 250
entries. The kernels are repetitive by design: read one and you have
read them all.

## The IR

Four types in [`graph.hpp`](../runtime/include/stanli/graph.hpp), kept
deliberately small:

- `Slot`: a value. An arena offset, a length, and whether it is a
  parameter. Parameter slots come first, in declaration order.
- `Op`: an opcode, up to six input slots, one output (occasionally
  two: a constraint transform also produces its Jacobian term),
  integer immediates (`idata`: index arrays, dimensions, outcome
  counts), an opaque payload (`udata`: today only ODE specifications),
  and a scratch window.
- `Graph`: the slots, the ops, and the pools that own the
  `idata`/`udata` arrays.
- `KernelCtx`: what a kernel sees at run time. A raw `double*` and a
  length for each input, the output, and each adjoint, plus scratch.
  Assembled once at bind, never rebuilt.

### The variant byte

In Stan, `y ~ normal(mu, sigma);` is allowed to drop additive
constants from the log density, and which terms count as constant
depends on which arguments are parameters: if `sigma` is data, its
`log(sigma)` term is a constant and gets dropped; if `sigma` is a
parameter, that term must stay. CmdStan's generated C++ makes this
decision through template types at compile time. stanli's kernels are
compiled before your model exists, so the decision is made at lowering
time instead and carried on the op, in the `variant` byte: bits 0-5
mark which arguments are being differentiated, bit 6 selects the
elementwise form (the output is each element's log density rather than
their sum), and bit 7 says whether `~`-style dropping (propto) is in
effect.

Bugs here are nasty in a specific way: the gradient stays perfect
while the log density is off by a constant. Sampling still works,
every gradient test passes, and only a direct comparison of `lp__`
against CmdStan notices.

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

The rules:

- **`in_adj[k].data` may be null.** Null means input k is data and
  nothing needs its derivative. Skip it.
- **Add numbers in the same order the reference does.** Floating-point
  addition is not associative: `(a+b)+c` and `a+(b+c)` can differ in
  the last bit. stanli promises bitwise agreement with CmdStan on much
  of the test corpus, so several kernels deliberately sum in a
  particular direction to match the stan-math code they are checked
  against. The trap: a reordered sum usually still passes the corpus
  check at its 1e-9 tolerance, and fails
  [`test_matvec`](../tests/test_matvec.cpp) at 1 ULP (one "unit in the
  last place": the gap between a double and the next representable
  double). Bitwise parity is a feature. Do not trade it for a tidier
  loop.
- **Scratch is private.** Sized at bind by `scratch_size`, never
  touched by any other op. The usual use: `forward` stashes partial
  derivatives there so `backward` does not have to recompute them.

## The register machine

The op list is fixed when the model loads. Two kinds of Stan code
cannot live under that rule:

- An ODE right-hand side must remain a callable function, because the
  solver decides at run time when, and how many times, to call it.
- `if (theta > 0) { ... }`, where `theta` is a parameter, picks its
  branch anew at every evaluation. A fixed list cannot contain "one of
  these two, decided later".

Both are handled by compiling the code in question, at load time, into
a program for a second, much simpler interpreter: a flat instruction
list over a numbered array of storage cells, i.e. a small register
machine ([`program.hpp`](../runtime/include/stanli/program.hpp)). One
function executes these programs, `run_program<T>`, templated on the
scalar type. With `T = double` it computes plain values. With `T =`
stan-math's `var`, the same instructions run under stan-math's own
autodiff, which builds its usual tape and yields the derivatives of
whichever branch actually executed. That is the same arithmetic
CmdStan's generated C++ performs for the same statement, so the
derivatives agree by construction.

Three front ends produce these programs:

- `compile_rhs` ([`ode_prog.cpp`](../runtime/src/ode_prog.cpp)) for
  ODE right-hand sides.
- `carve_islands` ([`island.cpp`](../runtime/src/island.cpp)) for long
  stretches of scalar ops that no pass could vectorize (an HMM
  recursion, say). This one is an optimization with a cost model:
  `STANLI_NO_ISLAND=1` turns it off, and `STANLI_ISLAND_ALWAYS=1`
  makes it skip the cost estimate.
- `lower_param_ifelse` / `lower_param_ternary`
  ([`lower.cpp`](../runtime/src/lower.cpp)) for parameter-dependent
  control flow. Not an optimization: without it the model does not
  compile.

The machine's instruction set covers ordinary arithmetic, every scalar
continuous density (the `DENSITY` instruction, backed by one table
shared with the MIR interpreter,
[`program_density.hpp`](../runtime/include/stanli/program_density.hpp)),
and, for any other scalar-result operation, a `CALL` instruction that
runs the graph's own kernel over a range of registers. A register
program can therefore say anything the graph can say about scalars,
and one unsupported function no longer splits a region in half.

Derivatives come in two ways. Parameter-dependent control flow and ODE
right-hand sides use the `var` replay described above. Islands instead
get a *generated* backward: `gen_adjoint`
([`adjoint.cpp`](../runtime/src/adjoint.cpp)) reads the forward
instruction list once, at load, and writes a second instruction list
that computes the derivatives directly, on plain doubles, allocating
nothing. Each derivative rule transcribes the matching stan-math
derivative expression for expression, so the two agree to the last bit
on almost every region, and `STANLI_NO_NATIVE_ADJ=1` switches back to
the replay, which is the oracle the generated programs are tested
against ([`test_adjoint.cpp`](../tests/test_adjoint.cpp)). One
exception: an island that branches on a parameter keeps the replay,
because reversing a branch needs the if/else shape the flat
instruction list has already thrown away.

## The sampler

stanli does not reimplement NUTS.
[`nuts.cpp`](../runtime/src/nuts.cpp) is short (about 240 lines)
because Stan's own classes do the sampling: it instantiates the same
`adapt_diag_e_nuts` CmdStan uses. What those lines actually do is
reproduce CmdStan's *configuration*: the same RNG streams consumed in
the same order, the same rules for accepting or rejecting an initial
point, the same adaptation schedule.

Configuration bugs are invisible to every gradient test: the gradient
can be perfect at every point while the chain visits different points.
The oracle is [`tools/sampler_trace.py`](../tools/sampler_trace.py):
run the same model with the same seed under stanli and CmdStan, and
compare the sampler diagnostic columns distributionally.

## Where the bugs have lived

The bugs worth recording share a shape: the graph looked right, the op
count did not change, and the numbers were silently wrong.

- **A destructive write ahead of a backward that re-reads its
  inputs.** The in-place pass turns "copy the vector, then modify one
  element" into "modify the element in place", which destroys the old
  value. The reverse sweep runs after all forward writes, so this is
  safe only if no earlier op's `backward` needs the destroyed value.
  The allowlist `backward_ignores_input_values` names the ops whose
  backward only routes adjoints and never re-reads its inputs; only
  those may sit in front of an in-place write, and
  [`test_pass_safety.cpp`](../tests/test_pass_safety.cpp) pins the
  property for each one. Before this rule existed, eight corpus models
  were wrong by up to 1.7e+05 relative, with their op counts
  unchanged.
- **Propto inside the register machine.** Which constants `~` drops
  depends on which arguments are parameters (see the variant byte).
  The register machine binds every value the same way and cannot make
  that distinction, so a `~` statement inside a parameter-conditional
  region is rejected, with a message telling the user to write
  `target += normal_lpdf(y | mu, 1)` instead. The bug this refusal
  prevents had a perfect gradient and an lp off by exactly
  log(2*pi)/2.
- **One slot both read and written by a region.** When an island's
  input slot is also its output slot, the producer's adjoint gets
  accumulated twice (measured effect: a gradient component off by
  exactly 1.0). An input that is also an output gets a fresh slot.
- **Reordered sums.** See the kernel contract. Cheap to do by
  accident, invisible in the corpus check, caught only by the ULP
  tests.
- **Modes that skip work.** `forward_value_only()` lets a kernel skip
  stashing partials. It is safe only because `gradient()` always runs
  a complete forward sweep first, so the skipped partials can never be
  consumed. If you add a mode that skips work, write down why the
  skipped work cannot survive into a reverse sweep.

Any change that claims a safety property is expected to come with a
mutation test: break the guard on purpose, watch the test fail, put
the guard back. If the test still passes with the guard broken, the
test is not testing the guard. One mechanical trap while doing this:
delete the object file and `touch` the source before rebuilding,
because make's file-timestamp granularity can hand you a stale binary
and a green run that tested nothing.

## Adding a stan-math function

First decide where the function needs to run.

**Interpreter only.** A function that appears only in transformed
data, ODE right-hand sides, or generated quantities never needs an op
or a kernel, because none of those places need stanli's gradient. Add
a branch to `eval_fun` in
[`mir_interp.hpp`](../runtime/include/stanli/mir_interp.hpp), or for
`_rng` functions to `rng_fun` in
[`wa_interp.cpp`](../runtime/src/wa_interp.cpp). Both fail loudly on
anything unhandled, and
`python3 harnesses/wa_coverage.py deps/posteriordb` reports what the
corpus needs that is still missing.

**An op.** Anything the log density touches must become an op, because
ops are the only path with a backward.

For a density, a distribution function (the `_cdf` family), or a
scalar math function, an op is one line: add the function to the
matching X-macro list in
[`optable.hpp`](../runtime/include/stanli/optable.hpp)
(`STANLI_SCALAR_DENSITY_LIST` and friends). An X-macro is a C
preprocessor idiom: the list is written once and expanded several
times with different definitions of `X`, so that one line generates
the opcode, the kernel, its registration, and the lowering entry
together, and they cannot drift apart. Then check the result against
CmdStan: `harnesses/fn_sweep.py deps/cmdstan --filter yourfn`
generates a one-function model, compiles and runs it under CmdStan,
and reports the worst ULP difference.

Not every stan-math function fits the one-line path. The generated
density kernels call stan-math's own code with a recording scalar type
that computes in plain doubles and has no arithmetic operators, so a
function whose implementation does arithmetic on the scalar type
itself (`von_mises_cdf`, `wiener_lpdf`) fails to compile rather than
silently losing derivatives. [`docs/coverage.md`](coverage.md) lists
which functions and why.

Everything else takes four steps:

1. An opcode in
   [`optable.hpp`](../runtime/include/stanli/optable.hpp) and a kernel
   in [`runtime/kernels/`](../runtime/kernels/). For a stan-math
   function with no native port, wrap the stan-math call with
   [`legacy.hpp`](../runtime/include/stanli/legacy.hpp) (examples in
   [`matrix_fns.cpp`](../runtime/kernels/matrix_fns.cpp)): the wrapper
   runs stan-math's own autodiff inside the kernel, so it is correct
   by construction, and it can be replaced with a native kernel if it
   ever shows up in a profile.
2. A lowering branch in the matching `lower_*_fn` group in
   [`lower.cpp`](../runtime/src/lower.cpp).
3. A test in the matching `tests/test_*.cpp`, asserting parity against
   the same stan-math call on `var` (the house pattern is in
   [`test_lower.cpp`](../tests/test_lower.cpp)).
4. Run `build-rel/dump_ops model.stan data.json` and read what
   actually lowered. The passes rewrite aggressively, and the op list
   is the only ground truth.

## Verifying a change

```
cmake --build build-rel -j8 && (cd build-rel && ctest)
python3 tools/verify_refs.py deps/posteriordb --check build-rel/stanli_check --jobs 8
```

The second command is the corpus replay: 128 models, log density and
every gradient component, compared against recorded CmdStan values.
That is the 119 runnable posteriordb posteriors plus the language
models in [`tests/stanc3/`](../tests/stanc3/README.md), which exercise
type and language constructs no real posterior uses. It is the
strongest oracle in the project. It
runs in CI on every push, and
[`tools/wasm_check.sh`](../tools/wasm_check.sh) drives the same replay
through the WASM build. A change that claims to be a pure refactor
should leave the replay's worst-deviation line exactly as it found it.

If the change touches a density tier or anything about propto, also
check the lite build, the smaller build variant that reports lp only
up to a constant ([`docs/lite-lp.md`](lite-lp.md)). CI does not cover
it, because that would mean a second full stan-math compile:

```
cmake -B build-lite -DCMAKE_BUILD_TYPE=Release -DSTANLI_LITE_LP=ON
cmake --build build-lite --target stanli_check -j8
python3 tools/verify_lite.py deps/posteriordb
python3 tools/verify_refs.py deps/posteriordb --check build-lite/stanli_check --no-lp
```

For sampler changes, use
[`tools/sampler_trace.py`](../tools/sampler_trace.py). For
generated-quantities coverage,
[`harnesses/wa_coverage.py`](../harnesses/wa_coverage.py). For any
performance claim, measure with `STANLI_PROFILE=1` before and after;
[`docs/benchmarks.md`](benchmarks.md) has the harnesses.

## Landing a change

`main` is protected; work lands by pull request.

```
git checkout -b my-change
git push -u origin my-change
gh pr create --fill
gh pr merge --auto --rebase
```

The merge happens when CI goes green. Release tags (`v*`, `npm-v*`)
are not gated by this; they point at commits that already passed on
main.

If a change plausibly touches Windows (build files, the C ABI surface,
`tools/exported_symbols.def`), run the wheel job on the branch before
merging:

```
gh workflow run wheels.yml --ref my-change
```

Release process and CI layout: [`README.md`](../README.md) under
"Releasing".
