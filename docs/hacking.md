# Hacking on stanli

A guide for contributors: how the pieces fit together, which file owns
what, and recipes for the most common changes. It assumes you know
Stan. It does not assume you have worked on a compiler or an
interpreter; the handful of terms of art are explained as they come up.

Companion documents:

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
- [`TESTING.md`](../TESTING.md) is the inventory of every oracle: what
  each one checks, what it gates, what it cannot see, and the known
  limits. Read it before changing anything a gate depends on.

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
  |  stanc3 parse/typecheck/Stan-Math transform (upstream OCaml)
  v  stanli-owned O1 + vectorize_loops policy (typed OCaml)
compiler/ocaml/stanli_pipeline.ml + portable_mir.ml
  |  canonical compact portable MIR v3
  v
decode_program (compact portable MIR)
  |  decoded into mir::Stmt / mir::Expr structs
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

1. **stanc3 and the stanli OCaml pipeline compile the model.** stanli builds
   stanc3 from its fork, [seantalts/stanc3](https://github.com/seantalts/stanc3)
   (`STANC3_SRC_REPO` and `STANC3_SRC_SHA` in
   [`deps/pins.env`](../deps/pins.env)): the stanc3 release CmdStan uses
   (`STANC3_UPSTREAM_SHA`) plus a few compiler commits on its `stanli`
   branch. It parses and typechecks with the same code CmdStan uses.
   [`stanli_pipeline.ml`](../compiler/ocaml/stanli_pipeline.ml) owns stanli's
   optimization policy: upstream O1 plus the separately selected upstream
   `vectorize_loops` pass. Every model gets O1, without O1's function
   inlining: user function calls stay calls in the MIR. The runtime inlines
   every call itself when it lowers the model. It knows the data then, so an
   argument that is data makes the matching formal data in the body, as the
   `data` qualifier would. Three O1 passes that change no portable MIR
   (`optimize_soa`, `allow_uninitialized_decls` and `block_fixing`) are off.
   The pipeline passes the selected typed MIR value to
   [`portable_mir.ml`](../compiler/ocaml/portable_mir.ml), which encodes the
   runtime's consumed slice. [`compiler/native/`](../compiler/native/) is the
   callback linked into native libraries; [`compiler/js/`](../compiler/js/)
   exports the same pipeline through js_of_ocaml and provides the CLI entry
   point cross-built for Windows as `stanli-compile.exe` for the parity gate
   (Windows ARM64 development builds compile it natively). The CLI prints MIR
   on stdout and messages for people on stderr; with `--messages-json PATH`
   it also writes `{"warnings": [...]}` to PATH, which the Python, R and
   native-tool callers read. `--passes none` gives the unoptimized MIR and
   `--passes LIST` runs only the named passes, for comparing the optimizer
   with unoptimized output.

   One of the fork's commits fixes integer folding. Upstream's partial
   evaluator folds with the host OCaml `int` width (63 bits natively and 32
   bits under js_of_ocaml), even though Stan integer literals and the portable
   contract are int32. The fork computes folds in `Int64`: representable int32
   results still fold, while an overflowing expression remains in MIR for
   runtime evaluation. The producer parity gate covers that no-fold path so
   the correction cannot silently disappear.

2. **[`mir_decode.cpp`](../runtime/src/mir_decode.cpp) decodes the MIR.**
   Native embedded compilation, the browser and R JavaScript compiler, and
   `stanli-compile` emit the stanli-owned compact portable format, and the
   runtime reads nothing else. It decodes into plain C++ structs
   (`mir::Stmt`, `mir::Expr`) and runs name resolution and validation
   (`mir_reader.cpp`). Anything it does not recognize is a loud error, never
   a guess. Stock stanc's `--debug-optimized-mir` s-expressions are refused
   with a message that names the supported compilers.

   The Windows x64 wheel and R runtime link the compiler into `stanli.dll`
   like the other releases, so no separate program is started (endpoint
   security software blocked a shipped `stanli-compile.exe`, #669). The
   `stanc-embed-windows` CI job builds the embed archive natively, and the
   `win_amd64` job links it with MinGW GCC. The cross-built
   `stanli-compile.exe` is still built for the parity gate but is not
   packaged. Native R without an embedded compiler, such as a source build,
   uses `stanli-compile` beside the runtime, and otherwise the bundled
   JavaScript compiler through V8. A failure from the selected compiler is
   surfaced without trying another.

3. **[`lower.cpp`](../runtime/src/lower.cpp) turns MIR into the op
   graph.** Along the way: transformed data is evaluated once, at
   load, using the MIR interpreter described below; loops whose bounds
   are data are unrolled, one copy of the body per iteration (the
   re-rolling pass will undo this); `~` statements are lowered with
   CmdStan's dropped-constant rules (see "the variant byte" below);
   and any statement whose control flow depends on a parameter is
   compiled for the register machine (its own section below).
   `write_array` is lowered from the prepared data, so in a build with
   threads and a model with enough to lower on both sides it runs on a
   second thread beside the log density and is joined before
   `compile_model` returns. `CompileOptions::parallel_write_array` (and
   `parallel_write_array` in the C ABI's `stanli_model_opts`, or
   `bench_grad --parallel-wa=`) chooses: `Off` lowers the two one after the
   other and `Force` always uses the second thread. Left at `Auto`, the
   developer switch decides: `STANLI_PARALLEL_WA=0` is `Off`, `1` or `force`
   is `Force`, and any other value but `auto` is `Off`.

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
   evaluation looks nothing up, and the executor allocates nothing; some
   kernels still allocate Eigen temporaries inside Stan Math code.

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
| [`compiler/portable_ir/SCHEMA.md`](../compiler/portable_ir/SCHEMA.md), [`compiler/ocaml/`](../compiler/ocaml/) | The stanli-owned portable MIR contract, shared optimization policy, and typed-OCaml encoder. |
| [`compiler/native/`](../compiler/native/), [`compiler/js/`](../compiler/js/) | Thin native callback, js_of_ocaml, and Windows CLI entry points around the shared OCaml pipeline. |
| [`runtime/src/lower.cpp`](../runtime/src/lower.cpp) | Lowering: transformed MIR in, op graph out. |
| [`runtime/src/mir_decode.cpp`](../runtime/src/mir_decode.cpp), [`portable_mir_reader.cpp`](../runtime/src/portable_mir_reader.cpp), [`mir_reader.cpp`](../runtime/src/mir_reader.cpp) | Decodes compact portable MIR into the MIR representation, then resolves names and validates it. |
| [`runtime/src/inplace.cpp`](../runtime/src/inplace.cpp), [`constfold.cpp`](../runtime/src/constfold.cpp), [`reroll.cpp`](../runtime/src/reroll.cpp), [`island.cpp`](../runtime/src/island.cpp) | The graph passes, in pipeline order. |
| [`runtime/src/executor.cpp`](../runtime/src/executor.cpp) | Runs the op list. `STANLI_PROFILE=1` prints per-opcode time accounting. |
| [`runtime/kernels/`](../runtime/kernels/) | The kernels: [`densities.cpp`](../runtime/kernels/densities.cpp), [`elementwise.cpp`](../runtime/kernels/elementwise.cpp), [`constrain.cpp`](../runtime/kernels/constrain.cpp), and friends. [`ode.cpp`](../runtime/kernels/ode.cpp) owns the canonical Stan Math solves and the direct all-double coupled RK path. [`matrix_fns.cpp`](../runtime/kernels/matrix_fns.cpp) and [`legacy_fns.cpp`](../runtime/kernels/legacy_fns.cpp) wrap stan-math functions that have no native kernel yet. |
| [`runtime/include/stanli/mir_interp.hpp`](../runtime/include/stanli/mir_interp.hpp) | The MIR interpreter: runs MIR directly, without lowering to ops. Used where the op graph cannot go: transformed data at load time, unsupported ODE right-hand-side fallbacks, interpreted `write_array`. |
| [`runtime/src/wa_interp.cpp`](../runtime/src/wa_interp.cpp) | Interpreted per-draw generated quantities for unsupported/container-result RNGs and dynamic geometry. Fixed-shape draw-dependent branches, checked one-level runtime indexing, and Viterbi backtracking compile; all 119 compiling corpus models currently have graph-backed write arrays. |
| [`runtime/include/stanli/program.hpp`](../runtime/include/stanli/program.hpp), [`mir_prog.hpp`](../runtime/include/stanli/mir_prog.hpp) | The register machine and its MIR front end. Its `DENSITY` instruction covers the 27 scalar continuous densities through [`program_density.hpp`](../runtime/include/stanli/program_density.hpp), one table shared with the MIR interpreter; its `CALL` instruction runs a graph kernel over a range of registers. |
| [`runtime/src/adjoint.cpp`](../runtime/src/adjoint.cpp) | `gen_adjoint`: differentiates a register program into a second register program, so an island's backward is a second cheap pass rather than a replay under stan-math's `var`. Owns the checkpoint analysis (save a register before a later write destroys a value a derivative rule needs). `STANLI_NO_NATIVE_ADJ=1` restores the replay, which is the oracle it is tested against ([`test_adjoint.cpp`](../tests/test_adjoint.cpp)). |
| [`runtime/src/nuts.cpp`](../runtime/src/nuts.cpp) | The sampler: Stan's own `adapt_diag_e_nuts`, configured to match CmdStan exactly. |
| [`runtime/src/capi.cpp`](../runtime/src/capi.cpp), [`capi.h`](../runtime/include/stanli/capi.h) | The C ABI. [`python/stanli/__init__.py`](../python/stanli/__init__.py) is a thin ctypes wrapper over it. |
| [`runtime/src/stanc_embed_c.cpp`](../runtime/src/stanc_embed_c.cpp), [`tools/stanc_embed/`](../tools/stanc_embed/) | The native OCaml-runtime bridge plus exact-source overlay, provenance, and the Windows cross-build and native embed tooling. |
| [`js/`](../js/), [`web/`](../web/) | The npm package (a one-call `sample()` over a worker) and the demo page. The worker loads `stanli-compiler.js` when a request carries Stan source. [`tools/build_web.sh`](../tools/build_web.sh) builds the page from the package, so the two cannot drift. |
| [`tools/`](../tools/) | Small programs and scripts; the important ones are listed below. |
| [`harnesses/`](../harnesses/) | Corpus sweeps that need a local posteriordb: [`fn_sweep.py`](../harnesses/fn_sweep.py), benchmarks. |
| [`tests/`](../tests/) | One `test_*.cpp` per subsystem, plus [`fixtures/`](../tests/fixtures/) with checked-in `.stan` sources. CMake generates ignored MIR before its consumers; run [`tools/gen_fixtures.sh`](../tools/gen_fixtures.sh) to do so explicitly. |

The tools you will reach for most:

- [`stanli_check`](../tools/stanli_check.cpp): one deterministic
  gradient evaluation, printed. With `--interp` it runs the whole model
  through the MIR interpreter instead, with a finite-difference gradient;
  [`tools/check_interp.py`](../tools/check_interp.py) compares the two
  ([the interpreter oracle](../TESTING.md#the-interpreter-oracle)).
- [`stanli_run`](../tools/stanli_run.cpp): a full sampling run,
  CmdStan-style CSV out.
- [`bench_grad`](../tools/bench_grad.cpp): gradient/forward timing. Pass
  `--prep` instead of an evaluation count to measure file-to-bound-executor
  preparation without warmup. It prints the preparation seconds, the
  parameter count, peak RSS in bytes after preparation (-1 where the
  platform has no getrusage), and the seconds of one gradient.
  `STANLI_PROFILE_PREP=1` splits that path into
  JSON/MIR parsing, every lowering pass, Executor construction, and binding.
  For compile timing, use `graph=compile stage=total`: the enclosing driver
  call also includes emitting the buffered profile rows.
- [`dump_ops`](../tools/dump_ops.cpp): print the op list a model
  lowered to. Usually the first thing to look at. Under each `LOOP` op it
  prints the loop body: the ops in control order, indented, in the same line
  format and with their storage class, between lines that show the `for`,
  `while`, `if` and `alias` structure. It prints the log density graph; pass
  `--write-array` after the op limit for the generated-quantities graph.
- `dump_ops model.mir data.json --execution-json`: JSON describing the
  final selected graph, nested register calls, structured sites, solver
  callback admission, write-array fallback and constrained initialization.
  It also captures interpreter constructions and selected preparation probes.
  `--forbid-mir` adds a strict diagnostic refusal, including preparation; it
  is intentionally stronger than the existing `STANLI_NO_INTERPRETER` policy.
  Constructions and probe entries are not instruction/evaluation counts.
- `STANLI_EXECUTION_REPORT=1`: emit the same manifest through the diagnostic
  sink at model compilation. `CompileOptions::execution_report`,
  `execution_report` in `stanli_model_opts`, `dump_ops --execution-report`
  and `stanli_function_set_execution_report` for a standalone function handle
  turn it on without the environment. The C API and BridgeStan emit a separate host
  selection event after write-array discovery, distinguishing an attached
  fallback from successful output discovery. Standalone function handles
  created with this flag emit interpreter construction traces on calls.
  The flag does not add clocks or counters inside graph/register instructions.
  C++ diagnostic callers can scope `ExecutionTrace` around initialization,
  write-array or callback evaluation; scopes are thread-local and nested
  scopes observe inclusive counts. Call `trace.check()` after any speculative
  catches when using its strict policy.

  Schema version 1 reports retained structure, not measured path frequency.
  Region `native_adj` describes generated derivative instructions, not machine
  code. Kernel derivative metadata belongs to the registered implementation;
  most kernels currently say `unclassified`, while graph softmax/log-softmax
  explicitly report `nested_tape`. Missing metadata never proves tape freedom.
  Logical shapes erased by lowering are `null`; flattened buffer sizes remain
  available. Dynamic solver/replay decisions are marked conditional. Structured
  sites on different branches are all listed, not claimed to have executed.
  Only the selected write-array route is listed; a partial/unselected graph
  is not added to its executing coverage. Use existing inclusive profiles for
  time attribution, and do not sum overlapping parent/child times.
- `STANLI_DUMP_PASSES=<dir>`: a developer switch read by anything that
  compiles a model. It writes the graph after every lowering pass to
  `<dir>/NN-<graph>-<stage>.txt`, numbered so `ls` sorts in pass order, plus
  the input MIR document as `00-mir.txt` (an encoded `STANLI5:` document).
  Diff two consecutive files to see exactly
  what one pass did. A pass that throws still leaves
  `NN-<graph>-FAILED-after-<stage>.txt` with the graph it died on.

  ```
  STANLI_DUMP_PASSES=/tmp/d ./build/stanli_check model.stan data.json
  diff /tmp/d/05-log_prob-constfold.txt /tmp/d/06-log_prob-reroll.txt
  ```

  `STANLI_DUMP_PASSES=-` writes to stdout instead, each dump bracketed by
  `;; <graph>:<stage>` and `;; end <graph>:<stage>`. No printed op or header
  line can begin with `;`, so a consumer can slice one stage back out.

  `STANLI_DUMP_STAGES=<list>` narrows the selection to a comma-separated set;
  unset or `all` is every stage. An entry is a bare stage name, which matches
  that stage in every graph, or `graph:stage`, which matches one. The input
  MIR is the stage `mir`. Sequence numbers still count the stages the filter
  dropped, so `05-log_prob-constfold.txt` is `05` under any selection.
  `stanli_check` takes the same two as `--dump-passes=<list>` and
  `--dump-dir=<dir>`; without a directory it dumps to stdout. It hands them
  to the runtime as `CompileOptions::dump_passes` and `dump_stages`, which a
  C++ caller can set directly, and refuses them with `--paths` or `--cross`.

  ```
  ./build/stanli_check model.stan data.json --dump-passes=log_prob:reroll
  ```

- [`dump_islands`](../tools/dump_islands.cpp): print every island's
  forward and adjoint programs, instruction by instruction.
- [`verify_refs.py`](../tools/verify_refs.py): replay the whole model
  corpus against recorded CmdStan values and report write_array coverage.
  Runs in CI; `--wa-headers` compares column names with a live CmdStan.
- [`verify_sample.py`](../tools/verify_sample.py): record those
  reference values (needs a CmdStan checkout).
- [`sampler_trace.py`](../tools/sampler_trace.py): compare sampler
  behavior against CmdStan.

For orientation, start with [`graph.hpp`](../runtime/include/stanli/graph.hpp)
(the IR) and [`optable.hpp`](../runtime/include/stanli/optable.hpp) (every
opcode, listed once). Lowering begins in [`lower.cpp`](../runtime/src/lower.cpp)
and continues in the `lower_*.cpp` files beside it, split by statements,
expressions, function calls and higher-order calls; a change to what a model
compiles to usually belongs there. [`executor.cpp`](../runtime/src/executor.cpp)
runs the result. What the op graph cannot express goes to the register machine
([`program.hpp`](../runtime/include/stanli/program.hpp),
[`mir_prog.hpp`](../runtime/include/stanli/mir_prog.hpp)) or the
[MIR interpreter](../runtime/include/stanli/mir_interp.hpp). The kernels are
repetitive by design: read one and you have read them all. Several files are
large, so run `wc -l` before opening one.

## The IR

Four types in [`graph.hpp`](../runtime/include/stanli/graph.hpp), kept
deliberately small:

- `Slot`: a value. An arena offset, a length, and whether it is a
  parameter. Parameter slots come first, in declaration order.
- `Op`: an opcode, up to six input slots, one output (occasionally
  two: a constraint transform also produces its Jacobian term),
  integer immediates (`idata`: index arrays, dimensions, outcome
  counts), and an opaque payload (`udata`: e.g. ODE specifications).
  Scratch offsets exist only while binding; the bound `KernelCtx` retains
  the scratch pointer, so graphs and executor copies do not store offsets.
- `Graph`: the slots, the ops, and the pools that own the
  `idata`/`udata` arrays. Construction uses separate mutable integer arrays;
  initial Executor construction packs live owned arrays into one immutable
  buffer shared by Graph/Executor copies. Numerical arenas remain private.
- `KernelCtx`: what a kernel sees at run time. A raw `double*` and a
  length for each input, the output, and each adjoint, plus scratch.
  Assembled once at bind, never rebuilt.

For metadata changes, build `bench_executor_clone` explicitly in separate
baseline and candidate Release build directories. Keep the baseline binary
unchanged, and clean-build the candidate after changing a layout header.
The model-independent scalar ADD and INDEX chains report graph construction,
integer finalization, binding, clone and gradient times, retained integer
elements/buffers, exact `Op` storage, and current/peak RSS in MiB.
Each clone is checked against an exact value and gradient before timing.

```sh
cmake --build build-base --target bench_executor_clone -j4
cmake --build build-candidate --target bench_executor_clone -j4
python3 tools/bench_executor_clone.py \
  build-base/bench_executor_clone build-candidate/bench_executor_clone \
  --ops 25000 --executors 1 8 32 --samples 12 --reps 20 \
  --json build-candidate/clone-layout.json
python3 tools/bench_executor_clone.py \
  build-base/bench_executor_clone build-candidate/bench_executor_clone \
  --workload index --ops 100000 --executors 1 8 --dead-payloads 100000 \
  --samples 12 --reps 20 --json build-candidate/clone-integers.json
```

The driver alternates fresh processes and reports medians and interquartile
ranges. RSS is descriptive: allocator retention and page rounding can mask
small live-storage savings. The ADD chain is a metadata stress case, not a
prediction of model-wide speedups; also run `bench_opcost` for density kernels
and `bench_grad` for a compiled-model canary. Timing runs should not overlap
builds or other benchmarks. INDEX gives each operation a live one-integer
payload; `--dead-payloads` adds unused four-integer arrays to either workload.
Include `finalize_ms` in setup comparisons: pooling is not free, even when
subsequent clones are cheaper. Per-graph storage counters include shared
integers, so summing them across clones overcounts physical storage.

### Integer payload ownership

`Graph::compact_idata()` is a finalization boundary, not a lowering pass.
Rewrite passes can hold pending Ops outside `graph.ops`, so compacting before
they return would invalidate their views. Ordinary owned `Op::idata` views
must name a construction buffer's exact base. Finalization validates those
views and lengths, packs each live buffer once, and releases dead arrays and
the outer pool capacity. All allocation precedes publication/rebinding.

Borrowed, interior, or invalid views cause the whole compaction to decline
without changing ownership. This preserves constant-folding subgraphs that
borrow parent metadata. Custom opaque payloads must own their integers rather
than borrow Graph-pool storage; finalization only rebinds views in `graph.ops`.
Even non-null zero-length views are conservatively retained or declined.

Compaction is one-shot once a shared buffer exists. Copying or extending a
finalized Graph is supported: copies share that buffer and deep-copy/rebind
any newly appended mutable arrays. Subsequent compaction leaves those new
arrays alone, so it never invalidates existing finalized views. Kernels keep
the same raw `const int*` access, with no extra hot-loop indirection.

### The variant byte

In Stan, `y ~ normal(mu, sigma);` is allowed to drop additive
constants from the log density, and which terms count as constant
depends on which arguments are parameters: if `sigma` is data, its
`log(sigma)` term is a constant and gets dropped; if `sigma` is a
parameter, that term must stay. CmdStan's generated C++ makes this
decision through template types at compile time. stanli's kernels are
compiled before your model exists, so the decision is made at lowering
time instead and carried on the op, in the `variant` byte. Density
kernels read and write it as a `DensityMode`
([`kernel_types.hpp`](../runtime/include/stanli/kernel_types.hpp)): which
arguments are being differentiated, whether the op writes the
elementwise form (each element's log density rather than their sum),
and whether `~`-style dropping (propto) is in effect. Every field is
written explicitly; a zero byte means no argument is active. The ODE,
DAE and quadrature kernels use `SolverActivity` the same way.

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
  loop. For the same reason the build compiles stan-math with
  `-ffp-contract=off`, so the compiler never fuses a multiply and an add.
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

Five front ends produce these programs:

- `compile_rhs` ([`ode_prog.cpp`](../runtime/src/ode_prog.cpp)) for
  ODE right-hand sides.
- `carve_islands` ([`island.cpp`](../runtime/src/island.cpp)) for long
  stretches of scalar ops that no pass could vectorize (an HMM
  recursion, say). This one is an optimization with a cost model:
  `STANLI_NO_ISLAND=1` turns it off, and `STANLI_ISLAND_ALWAYS=1`
  makes it skip the cost estimate.
- `lower_runtime_ifelse` / `lower_runtime_ternary`
  ([`lower_stmt.cpp`](../runtime/src/lower_stmt.cpp)) for control flow
  whose condition is unavailable while the graph is built (`if`, `while`
  and the ternary operator). This includes genuine parameter branches and
  data-only predicates computed in graph-local storage. It is not an
  optimization: without it those models do not compile.
- `lower_region_map` ([`lower_region_map.cpp`](../runtime/src/lower_region_map.cpp))
  for long loops with parameter-dependent control whose body only adds to
  the target.
- Standalone Stan functions
  ([`function.cpp`](../runtime/src/function.cpp)), called from the C API
  and R and Python bindings.

The machine's instruction set covers ordinary arithmetic, every scalar
continuous density (the `DENSITY` instruction, backed by one table
shared with the MIR interpreter,
[`program_density.hpp`](../runtime/include/stanli/program_density.hpp)),
and, for any other scalar-result operation, a `CALL` instruction that
runs the graph's own kernel over a range of registers. A register
program can therefore say anything the graph can say about scalars,
and one unsupported function no longer splits a region in half.

Parameter-dependent control flow uses the `var` replay described above.
Islands instead get a *generated* backward: `gen_adjoint`
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

ODE right-hand sides use both mechanisms. For active sensitivities, the
canonical path still replays the compiled RHS under stan-math's `var`, and its
Stan Math solve remains the oracle for every solver and shape. A narrower,
default-on RK45/CKRK path clones a branchless compiled RHS and generates a
backward for that clone. At each solver callback
it computes `f`, `J_y`, and `J_theta` on doubles, then advances one coupled
state containing the primal system and the active initial-state and parameter
sensitivity lanes. The coupled vector uses the same Boost tableau, controller,
error norm, observation schedule, and scalar accumulation order as the pinned
Stan Math implementation. Its observer writes values and the complete solution
Jacobian directly into the ODE op's output and scratch; the graph backward is
unchanged.

This ODE path fails closed. Only an explicit exact-grouping opcode whitelist is
eligible; runtime branches, `DOT`, signed-zero-sensitive `FMAX`/`FMIN`, calls,
and any unfamiliar instruction retain the canonical solve. BDF, Adams,
data-only solves, and malformed runtime shapes do too. The canonical program
is not modified when the derivative clone receives checkpoint saves.
`STANLI_NO_ODE_DIRECT_RK=1`, set before the model is loaded, selects the oracle
without suppressing payload construction or eligibility diagnostics. Selection
is recorded in the ODE specification at lowering, so gradient evaluation does
not read the environment.

Do not confuse this with the rejected Phase 0 callback bridge. That experiment
wrapped generated rows in `precomputed_gradients_vari` callback results and
failed its speed gate. The direct RK path creates no callback vars or
precomputed-gradient nodes: generated reverse is only a local-Jacobian provider
inside an all-double coupled solve. The bridge remains stopped. Admission was
based on exact macOS arm64 model and synthetic results plus integrated 1.86x
and 1.80x oracle/direct gains on Lotka--Volterra and soil. Linux x86-64 exact
default/oracle agreement and the focused ODE tests remain a merge gate; do not
turn an architecture disagreement into a tolerance.

## The sampler

stanli does not reimplement NUTS.
[`nuts.cpp`](../runtime/src/nuts.cpp) delegates sampling to Stan's classes:
it instantiates the same
`adapt_diag_e_nuts` CmdStan uses. The driver reproduces CmdStan's initial
step size, dual-averaging center, initialization acceptance rules and
adaptation schedule. In particular, initial dual averaging is centered
on the requested step size before the step-size search.

[`test_sampler_parity`](../tests/test_sampler_parity.cpp) compares every
draw and diagnostic bitwise with Stan's actual sampling service, using
the same executor for numerical evaluation in both drivers. It covers
short and metric-adapting warmup, thinning, saved warmup, different chain
IDs, zero initialization, random generated quantities and continuation
after generated-quantity domain errors. The CLI and full-output C API
evaluate generated quantities on the sampler's RNG between saved draws,
matching CmdStan's stream schedule. Parameter-only APIs skip generated
quantities and can therefore follow different trajectories. This isolates
configuration from compiler numerics. Cross-compiler traces can still
split after tiny numerical differences.

Configuration bugs are invisible to every gradient test: the gradient
can be perfect at every point while the chain visits different points.
The broader oracle is [`tools/sampler_trace.py`](../tools/sampler_trace.py):
run the same model with the same seed under stanli and CmdStan, and
compare the sampler diagnostic columns distributionally.

## Where the bugs have lived

The bugs worth recording share a shape: the graph looked right, the op
count did not change, and the numbers were silently wrong.

- **A destructive write whose producer or an earlier reader re-reads
  values in backward.** The in-place pass turns "copy the vector, then
  modify it" into "modify it in place", which destroys the old value.
  The reverse sweep runs after all forward writes, so this is safe only
  if neither the op that produced the current version nor any earlier
  reader needs the destroyed input or output values. The allowlist
  `backward_ignores_values` names the ops whose backward only routes
  adjoints or reads scratch; both sides of that proof must be on it, and
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
anything unhandled, and `python3 tools/verify_refs.py deps/posteriordb
--wa-report --check build-rel/stanli_check` reports what the corpus needs
that is still missing.

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
silently losing derivatives. Those go on `STANLI_TAIL_CDF_LIST` or
beside `wiener` instead, and get a nested var tape in
[`matrix_fns.cpp`](../runtime/kernels/matrix_fns.cpp);
[`docs/coverage.md`](coverage.md) says how to tell which list a
function belongs on.

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

## C++ entry points

Native builds with embedded stanc expose a value-only function entry
point. `Function` compiles the source once; later calls bind named
`DataMap` arguments to the Stan formals and return a `DataMap::Entry`
with integer identity and logical dimensions.

```cpp
#include <stanli/function.hpp>

const std::string source = R"stan(
functions {
  vector affine(vector x, real a, real b) {
    return a * x + b;
  }
}
model {}
)stan";

stanli::Function affine(source, "affine");
stanli::DataMap args;
args.set_real_array("x", {1, 2, 4});
args.set_real("a", 2.5);
args.set_real("b", -1);
stanli::DataMap::Entry result = affine(args);  // {1.5, 4, 9}
```

`Function::from_mir(mir, name)` skips source compilation for cached
portable MIR and works in builds without embedded stanc.
Arguments are matched by formal name. This surface evaluates pure,
value-returning functions on doubles; `_lp`, void, RNG, and autodiff
entry points are not part of it.

Native C++, C, Python, R and `stanli_run` support opt-in within-chain
`reduce_sum` parallelism with reusable per-chain workers
([`native-reduce-sum.md`](native-reduce-sum.md)). Pass
`threads_per_chain=N` when you build the model (`stanli.Model()` in Python,
`stanli_model()` in R), or `--threads-per-chain N` to the CLI; the default is
one.

## Building

Developer builds on Linux and macOS use `clang` and `clang++` with
C++17, matching pull-request validation. The Linux and Windows release
wheels keep their GCC toolchains, and macOS uses AppleClang. One command
fetches the pinned dependencies, builds, and runs the tests:

```
./tools/dev_setup.sh               # build + tests + source-pinned stanc3, embedded in the library
./tools/dev_setup.sh --no-embed    # standalone stanli-compile instead (required on Windows ARM64, which has no release build)
./tools/dev_setup.sh --corpus      # + posteriordb and CmdStan
./tools/dev_setup.sh --conformance # + the Stan conformance reference stack
./tools/dev_setup.sh --all
```

Embedding the compiler is the default, and `--embed` selects it
explicitly. With `--no-embed`, setup builds `stanli-compile` from the
same pipeline and CMake copies it beside `stanli_check` and `stanli_run`,
including in `cmake --install` deployments; keep those executables
together when moving an installation. `--all` respects `--no-embed` in
either order. The tools also accept `--stanli-compile PATH`.
`--conformance` stages the standalone compiler with the
Python library when embedding is disabled, and makes the differential
Stan language sweep runnable locally; see
[`harnesses/conformance/README.md`](../harnesses/conformance/README.md).

The embedded compiler object and the vectorization probe live in
`deps/stanc3/embed/<key>/`, where `<key>` hashes the stanc3 pin, the OCaml
version and the files in `compiler/native`, `compiler/ocaml` and
`tools/stanc_embed`. Switching branches selects another directory instead of
overwriting the object. The OCaml headers and C link libraries it was built
against sit beside it. Configure with `-DSTANLI_EMBED_STANC=ON` to link the
object for the current tree, as `dev_setup.sh` does; the default, `OFF`,
builds without embedding. Every configure checks the object's provenance stamp and fails with the
rebuild command if it is missing or stale. A change to any of those inputs
reruns the configure on the next `cmake --build`. When
`STANLI_STANC_EXECUTABLE` is left at `deps/stanc3/stanc`, configure also
requires its `stanc.src` to match `STANC3_SRC_SHA` in `deps/pins.env`.
CmdStan references compile with `deps/stanc3/stanc-pinned` instead, built
from `STANC3_UPSTREAM_SHA` by
[`harnesses/conformance/build_stanc.sh`](../harnesses/conformance/build_stanc.sh),
so a fork commit never changes the oracle.
Old key directories are not pruned; delete `deps/stanc3/embed` to reclaim
the space.

On Windows, run `bash tools/dev_setup.sh` from Git Bash or MSYS2 Bash.
Setup reuses the installation providing `pacman` on `PATH`, or checks
`C:/msys64` and installs `MSYS2.MSYS2` with winget if absent. It prepends
the CLANG64 directories on x86_64, or the CLANGARM64 directories on ARM64,
to `PATH`. Pacman provides Git, Python, make, Clang, LLD and CMake, and a
missing opam comes from winget. Windows ARM64 needs `--no-embed` because
its OCaml compiler runs under x64 emulation while the runtime is native
ARM64. On x64, `tools/dev_setup.sh --embed --no-build` under MSYS2 CLANG64
is also how CI builds the embed archive that the release wheel links.
MinGW `RelWithDebInfo` builds use `-g1` to keep objects and
`.exe.debug` files small while retaining source lines and backtraces; use
`-DCMAKE_BUILD_TYPE=Debug` for full variable and type information.

Full native Windows setup runs in CI after pushes to `main` and on manual
dispatch, covering x64 embedding and ARM64 standalone deployment. Pull
requests run the fast setup contract tests, the `win_amd64` wheel build
and Windows compiler parity instead. Windows CI caches the OCaml toolchain and validated compiler
artifacts, keyed on compiler versions and the source pin, before building
C++. A failed compiler launch keeps the cache and reports the error; only
a successfully reported incompatible version replaces the switch. CI uses
`--no-build` for compiler preparation and `--no-test` to save the build
before a separate test step.

Or manually:

```
./tools/dev_setup.sh --no-build
build_jobs=$(tools/build_jobs.sh)
cmake -B build -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DSTANLI_DEBUG_SWITCHES=ON
cmake --build build --parallel "$build_jobs"
ctest --test-dir build --parallel "$build_jobs"
```

The runtime's `STANLI_*` environment variables that turn a pass off, force
an engine, set a tuning limit or print a diagnostic, such as those in
[OPTIMIZATIONS.md](../runtime/src/OPTIMIZATIONS.md), are developer switches,
read through `debug_env()` in
[`debug_switch.hpp`](../runtime/include/stanli/debug_switch.hpp). Build and
tool variables such as `STANLI_JOBS` and `STANLI_PROFILE` are not. CMake's
`STANLI_DEBUG_SWITCHES` compiles them in. It defaults to OFF, which is what
the wheels, the R runtime and the browser builds ship: there a set switch is
ignored, with a one-time notice. `tools/dev_setup.sh` configures both of its
builds ON. Tests that set a switch carry the CTest label `debug_switches`. In
an OFF build a case that needs a switch skips itself and the rest of the test
runs; tests where no case can run are disabled. `ctest -L debug_switches` runs
them all in an ON build. See [Tests that need developer switches](#tests-that-need-developer-switches). `STANLI_NO_INTERPRETER`, `STANLI_ISA` and `STANLI_NO_FUSED_DENSITY` are not
developer switches and work in every build.

`tools/build_jobs.sh` caps parallelism by CPU count and usable RAM: 4 GiB
per concurrent Stan Math compile on macOS and Windows, 6 GiB elsewhere,
with container limits honored. Override the result with `STANLI_JOBS=12`,
or raise the per-job budget with `STANLI_JOB_MEMORY_GIB` (the
AddressSanitizer recipe below uses 12 GiB). The measured scaling curve is
in [`notes/performance/build-performance.md`](../notes/performance/build-performance.md).

## Verifying a change

```
build_jobs=$(tools/build_jobs.sh)
cmake --build build-rel --parallel "$build_jobs" && \
  ctest --test-dir build-rel --parallel "$build_jobs"
python3 tools/verify_refs.py deps/posteriordb --check build-rel/stanli_check --jobs 8
```

The second command is the corpus replay: every model with a recorded reference
([TESTING.md](../TESTING.md#every-model) lists them), log density, every
gradient component and every `write_array` value, compared against recorded
CmdStan values.
That is the 119 runnable posteriordb posteriors, the lowering
regressions in [`tests/regressions/`](../tests/regressions/README.md), the brms
output in [`tests/brms/`](../tests/brms/README.md), cogmod's custom
brms families in [`tests/cogmod/`](../tests/cogmod/README.md), ctsem's
state-space template in [`tests/ctsem/`](../tests/ctsem/README.md),
programs from stan-dev/example-models in
[`tests/examplemodels/`](../tests/examplemodels/README.md), programs from the
Stan User's Guide in [`tests/stanguide/`](../tests/stanguide/README.md), and
models from CRAN packages, one directory per package (such as
[`tests/oncobayes2/`](../tests/oncobayes2/README.md)), listed in
`CRAN_PACKAGES` in [`tools/corpus_inventory.py`](../tools/corpus_inventory.py). It is the
strongest oracle in the project. It
runs in CI on every push, and
[`tools/wasm_check.sh`](../tools/wasm_check.sh) drives the same replay
through the WASM build. A change that claims to be a pure refactor
should leave the replay's worst-deviation line exactly as it found it.
For kernel changes, see [the rule in `TESTING.md`](../TESTING.md#the-rule):
every value within 10 ULP of CmdStan or listed in the ULP ledger, and no more
than 2 ULP of movement from a change that does not reorder a sum.

To add a CRAN package, put its `.stan`/`.json` pairs, a README and
`licenses/` in `tests/<package>/` and its generator in
`tools/gen_<package>_data.R`, add the name to `CRAN_PACKAGES`, record the
references with [`tools/verify_sample.py`](../tools/verify_sample.py), and
regenerate the docs.

If the change touches raw pointers, an `Eigen::Map` over a buffer
somebody else sized, or the island register file, run the suite with
AddressSanitizer as well. CI does this after merges and nightly, and it is
one flag locally. Its compiler uses much more memory than Release, and the
build compiles Stan source through `stanli-compile`, which
`./tools/dev_setup.sh --no-embed` builds:

```
asan_jobs=$(STANLI_JOB_MEMORY_GIB=12 tools/build_jobs.sh)
cmake -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSTANLI_SANITIZE=address,signed-integer-overflow \
  -DSTANLI_DEBUG_SWITCHES=ON \
  -DSTANLI_PORTABLE_COMPILER="$PWD/deps/stanc3/stanli-compile"
cmake --build build-asan --parallel "$asan_jobs" && \
  ctest --test-dir build-asan --parallel "$asan_jobs"
```

`address,undefined` also passes clean. What ASan cannot see is a stale
`var`: stan-math's arena recovers a nested tape by rewinding a bump
pointer, so reading a `vari` from a tape that was already recovered
lands inside a live allocation and reports nothing.

If the change reads sizes, indices or bytes that a caller supplies, run the
fuzzers in [`tests/fuzz`](../tests/fuzz/) too. They need LLVM's Clang,
because Apple's Clang and GCC ship no libFuzzer. On Linux that is `clang`;
on macOS, use Homebrew's `llvm` (`llvm=$(brew --prefix llvm)/bin/`):

```
cmake -B build-fuzz -DCMAKE_BUILD_TYPE=None -DCMAKE_CXX_FLAGS="-O1 -g1" \
  -DCMAKE_C_COMPILER="${llvm}clang" -DCMAKE_CXX_COMPILER="${llvm}clang++" \
  -DSTANLI_SANITIZE=address,signed-integer-overflow -DSTANLI_FUZZ=ON
cmake --build build-fuzz --target fuzz_manifest fuzz_lowrank fuzz_data
mkdir -p /tmp/fuzz-manifest
build-fuzz/fuzz_manifest -max_total_time=300 \
  -dict=tests/fuzz/manifest.dict /tmp/fuzz-manifest tests/fuzz/corpus/manifest
```

Name a scratch directory first, as here, so that new inputs go there and not
into the checked-in seeds. A crash writes a `crash-*` file. Once the bug is
fixed, add that file to the target's directory in `tests/fuzz/corpus/`, and
CTest replays it from then on.

If the change touches `runtime/kernels/density_fused.cpp`, build with the
Stan Math kernels kept, which is the default of a plain `cmake -B build`
(`-DSTANLI_STAN_DENSITY_ORACLE=ON`), and run `test_density_fused`. It compares
each fused kernel with Stan Math's own over every activity mask and shape and
requires bitwise equal results, except 2 ULP for normal and cauchy.
`tools/dev_setup.sh` configures `build-rel` with the option OFF, as the release scripts and CI do, so the
corpus replay there runs the fused kernels alone; its `build` keeps them.
`STANLI_NO_FUSED_DENSITY=1` selects Stan Math's kernels in an ON build and
prints a notice, then is ignored, in an OFF build.

On macOS, `STANLI_ACCELERATE` (on by default on Apple silicon, off by
default on Intel Macs, unavailable on other systems) links Apple's
Accelerate framework. A pass at the end of lowering gives a
matrix product, a matrix-vector product with data, a Cholesky factor, a
`multi_normal_cholesky` with data `y` and `mu`, a `multi_normal` of one
vector with an active covariance, or a `normal_id_glm` with data `y` and `X`
and an active `beta` an accelerated opcode when the op is at least as large
as that operation's threshold; the kernel then calls BLAS or LAPACK. Every smaller op runs the same code as a build with
`-DSTANLI_ACCELERATE=OFF`, and so do Linux, Windows and the browser builds.
The thresholds and the sweep behind them are in
[`notes/performance/2026-11-26-accelerate-thresholds.md`](../notes/performance/2026-11-26-accelerate-thresholds.md).
Every accelerated BLAS or LAPACK call runs on the calling thread alone
(`BLASSetThreading`, macOS 15 or later), and the thread's previous setting
is restored afterwards, so results are the same in every run. The model
option `parallel_blas` (C API `stanli_model_opts`, Python `Model`, R
`stanli_model()`) lets a model's accelerated calls use Accelerate's own
threads while the chains that run in parallel leave cores free: twice the
number of parallel chains at most the number of performance cores, with a
single gradient outside a sampler counted as one chain. `dpotri` comes
from Accelerate's newer LAPACK interface (`dpotri$NEWLAPACK`, a weak import,
since it exists only from macOS 13.3); the other routines keep their legacy
names. With Accelerate's threads, the legacy `dpotri_` (through `dlauum`)
sometimes returns a wrong inverse: 265 of 192,000 calls at n = 151 on macOS
26.6, and 1 in 18 to 1 in 45 at n = 512. The newer one gave the right
inverse in all 192,000 calls at n = 151 and in all 100,000 at n = 64 to
2048 (`tests/test_blas_determinism.cpp`).
`tools/stress_blas_threads.cpp` (`make stress_blas_threads`) makes the calls
that the kernels make, with the same arguments, at many shapes from the
thresholds to n = 4096 with threads on (`--kernel-shapes`, `--sweep`), and
compares each result with the single-threaded one. With threads, `dgemm`,
`dsymm`, `dpotrf` and `dpotri` gave other bits for some shapes, at
rounding level ([the thresholds note](../notes/performance/2026-11-26-accelerate-thresholds.md#threads)),
so with `parallel_blas` on, results can differ by rounding from those with
it off, and with the number of chains that run in parallel. The
lowered program carries the option, and the program dump names it. On macOS
11 to 14, which lack `BLASSetThreading`, the selection pass keeps every op
on the plain kernels (`kAccelerateWithoutThreadControl` in
`runtime/include/stanli/dense_blas.hpp`), so those systems get no speedup
and the results of a build without the option. In a build with developer
switches, `STANLI_NO_ACCELERATE` keeps every op on the plain kernels and
`STANLI_PERFORMANCE_CORES` sets the core count that `parallel_blas` uses.

For sampler changes, use
[`tools/sampler_trace.py`](../tools/sampler_trace.py). For
generated-quantities coverage, run the verifier with `--wa-report`.
For a live CmdStan column-name comparison, use `--wa-headers deps/cmdstan`.
Both modes default to `build-rel/stanli_check`. For any
performance claim, measure with `STANLI_PROFILE=1` before and after for
evaluation work, or `STANLI_PROFILE_PREP=1` for compilation and binding;
[`docs/benchmarks.md`](benchmarks.md) has the harnesses. When the question is
what a pass did rather than what it cost, `STANLI_DUMP_PASSES=<dir>` writes
one graph dump per stage for diffing.

### Updating the ULP ledger

[`tests/ulp_ledger.json`](../tests/ulp_ledger.json) lists every value that is
more than 10 ULP from CmdStan ([TESTING.md](../TESTING.md#the-rule)). Its keys
are the platform, then the corpus, then the model. The corpus is `main` or the
name of a local corpus, with ` --fixture-passes` added for a replay that
compiles with the O1 passes of the MIR fixtures.

An entry covers one value, such as `lp`, `grad[3]` or `wa mu.2`. When a model
has more than 8 such values of one kind, one `grad` or `wa` entry covers every
value of that kind that has no entry of its own.

Each entry has three fields:

- `bound` is the largest distance measured, in ULP.
- `reason` is the cause.
- `closer` says which engine is closer to the 80-digit value. It is `stanli`,
  `cmdstan` or `unknown`.

The ledger's `true_values` key holds the true values
([TESTING.md](../TESTING.md#true-values)). They do not depend on the
platform, so its keys are the corpus, then the model, then the label of a
platform entry. Each entry has four fields:

- `points` maps each point to the values that the entry covers there. Each
  value has the true value as a decimal string (`value`) and the scale `S`
  of its distance (`scale`). An entry for one value has one value per point.
  A `grad` or `wa` entry lists every value that it covered when the tool
  wrote it. A point where `S` is 0 has no values, and the check against
  CmdStan stays on for it.
- `bound` is the largest distance of stanli from the true value, in ULP of
  `S`, over the points.
- `method` is `fd` for a gradient entry (a full finite-difference gradient)
  and `direct` for `lp` and outputs.
- `platform` is where the bound was measured. It must have the platform
  entry, with `closer` set to `stanli`.

After you change the ledger, run `python3 tools/gen_docs.py` to rewrite the
table in TESTING.md.

The bounds come only from a tool run:

    tools/verify_refs.py deps/posteriordb --update-ulp-ledger
    tools/verify_refs.py --corpus NAME [--fixture-passes] --update-ulp-ledger

The run writes nothing unless every model passes every gate, including the
ledger's current bounds. It lowers an entry to the new measurement and
deletes an entry that is back within 10. It never raises a bound: a value
over its bound fails the replay, and raising it means editing the entry by
hand in a commit that says why. On a platform or corpus with no section yet,
the gate is off, and the run creates the section with an entry for every
value over 10. Such a generated entry's reason says only where it was
measured; a person replaces it with the cause. A replay that measures half
the bound or less prints a `ULP LEDGER` note asking for a rerun to tighten it.

The run does not hold a value with a true value to CmdStan, so it never
lowers, deletes or adds a platform entry for that value. It lowers the bound
of a true value to the new measurement, but only on the platform in the
entry's `platform` field. A replay that measures half that bound or less
prints a `TRUE VALUE` note. To fill `true_values` from a set of 80-digit
results, run

    PYTHONPATH=... tools/hp/ledger_accuracy.py HP_DIR --write-true-values

on macOS arm64 ([tools/hp](../tools/hp/README.md#the-ulp-ledger)).

Inside a section, a value over 10 that no entry covers fails the gate, so a
model that is new to the corpus, or a value that moved for a known reason,
gets its entries from a separate run:

    tools/verify_refs.py deps/posteriordb MODEL... --add-ulp-entries REASONS.json

`REASONS.json` maps each named model to `{label: reason}`. The run replays
those models without their ULP gate, and every other model with it, and
replaces the named models' entries. The same run raises an entry of a model
that has moved for a known reason, because the ledger cannot tell that from
a regression: only the person who names the model can. A value over 10
needs a reason in the file, unless the model has an entry for it that the
value is within, which keeps its reason. The run writes nothing unless every
model passes the other gates and every value over 10 has a reason, and no
reason names a value within 10. A first run with an empty `{}` per model
lists the values that need one.
`--corpus NAME` takes the same flag.
`REASONS.json` may also give `{"reason": ..., "closer": ...}` for a label. A
plain string means `closer` is `unknown`. Score an entry against the 80-digit
value with [`tools/hp/ledger_accuracy.py`](../tools/hp/README.md).

Linux and Windows have no section yet, so the 10 ULP rule against CmdStan is
not enforced there. The check against true values runs there. Record a
section by running the commands above on that platform.

### Tests that need developer switches

A test that sets a switch is listed in `STANLI_DEBUG_SWITCH_TESTS` in
[`CMakeLists.txt`](../CMakeLists.txt) and carries the CTest label
`debug_switches`. Such a test builds and runs in every configuration. In an
OFF build, a case that needs a switch skips itself: it starts with
`SKIP_WITHOUT_DEBUG_SWITCHES()` or tests `debug_switches_available()`, both
in [`tests/env_helpers.hpp`](../tests/env_helpers.hpp), and prints a `SKIP` line,
while the cases that need no switch run. A case that sets a switch and does
not skip still runs in an OFF build, where the switch is ignored, so it passes
only if its checks hold without it. Some cases that compare a run with and
without a switch are like that today: they pass by comparing the default with
itself, and only an ON build tests them.

Thirteen tests and the 22 cogmod runs of the cross-path matrix
(`test_cross_path_cm_*`) are disabled in an OFF build, because almost every
case in them needs a switch. They are in `STANLI_DEBUG_SWITCH_ONLY_TESTS`, and CTest
reports `Not Run (Disabled)`: `test_cross_path`, `test_structured_loop`,
`test_region_map`, `test_wa_parallel_switches`, `test_pass_dump_switches`,
`test_prep_profile_reroll_dispositions`, `test_prep_work`, `test_ctsem_engine`
and `test_data_loop_memo`. The four `test_structured_loop_large_*` cases are
also disabled.
CTest reports those 35 as disabled in an OFF build.

The structured-loop suite runs its four large-graph cases in separate
processes. They share the `structured_large_graph` CTest resource lock, so
parallel runs release one case's graphs before starting another. The ordinary
suite and the switch-free forward suite remain separate. Running
`test_structured_loop` directly without arguments still runs every case.

CI runs CTest in the shipped OFF `build-rel/` on every platform, so the
switch-free cases of the pass, lowering, write_array and sampler-parity tests
run on macOS, Linux aarch64 and Windows x64 as well. The manylinux job's
differential step builds ON and runs `ctest -L debug_switches`, which is
where the skipped cases and the thirteen disabled tests run. The
AddressSanitizer job and the Windows ARM64 setup job, which run after a merge,
also build ON. Locally:

```sh
cmake -B build -DSTANLI_DEBUG_SWITCHES=ON
cmake --build build --parallel "$(tools/build_jobs.sh)"
ctest --test-dir build -L debug_switches
```

A test that needs only pass dumps, an execution report or a write_array
thread policy sets the typed fields instead, and runs in every build. The
fields are `CompileOptions::dump_passes`, `dump_stages`, `execution_report`
and `parallel_write_array`; the last two are also in `stanli_model_opts`.
`test_wa_parallel --switches` and `test_pass_dump --switches` are the labeled
halves that check the environment switches themselves.

### Other checks to run by hand

The cross-path, sampler and recording tools use release-build targets:

```sh
./tools/dev_setup.sh --conformance   # sweep stack and .venv-conformance
cmake --build build-rel --parallel "$(tools/build_jobs.sh)" \
  --target stanli_check dump_ops stanli_shared
build-rel/stanli_check model.stan data.json --cross        # cross-path
python3 tools/check_corpus_sampling.py --build build-rel   # sampling smoke
python3 tools/sampler_trace.py deps/cmdstan deps/posteriordb eight_schools_noncentered
python3 tools/verify_sample.py deps/cmdstan --corpus functions  # re-record
python3 tools/check_stanc3_good.py --record     # stanc3 good references
cp build-rel/libstanli.* python/stanli/_bin/   # the sweep loads this copy
.venv-conformance/bin/python harnesses/stan_conformance.py \
  --stanc deps/stanc3/stanc-pinned --cmdstan deps/cmdstan \
  --build build-rel --stanli-pythonpath python --filter log1p
```

The WebAssembly replay also needs Node, Emscripten and stancjs's dependencies:

```sh
./tools/build_web.sh
python3 tools/verify_refs.py deps/posteriordb \
  --check tools/wasm_check.sh --target-platform 'WebAssembly wasm32' \
  --no-wa --skip nn_rbm1bJ100
```

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

Pull requests keep one Linux native build, CTest, recorded CmdStan replay,
installed Python/R checks, and native/JavaScript compiler parity. Performance
comparisons, Windows/compiler and browser runtime builds, and the full
R-version matrix run after merge. See
[`TESTING.md`](../TESTING.md#checks-run-before-and-after-merge) for the exact
split and [`AGENTS.md`](../AGENTS.md) for project priorities.

When a change needs additional platform or optimization evidence before
landing, run its focused check or dispatch the full workflow on the branch:

```
gh workflow run wheels.yml --ref my-change
```

Release process and CI layout: [`RELEASING.md`](../RELEASING.md).
