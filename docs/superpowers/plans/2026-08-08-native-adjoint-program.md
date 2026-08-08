# Native adjoint program: differentiate the register machine without vars

**Goal:** an island's backward pass currently *re-executes the whole
program under `stan::math::var`*. That replay is bitwise-correct by
construction — it is the same var arithmetic CmdStan's generated code
runs — and it therefore costs what CmdStan costs, plus the register-file
seeding on top. Generate a second register program at compile time that
computes the gradient directly on doubles, and the island's backward
stops being a var-tape build-and-walk and becomes a second double pass.

This is the one lever the benchmarks doc names twice and has never
pulled: "what would move the rest of this class is not fewer dispatches
but a cheaper backward: a native adjoint program, generated alongside the
forward one instead of replayed under nested autodiff."

## Why the current backward caps the sequential class

`run_island<T>` (island.hpp) is templated on the scalar:

- **forward:** `T = double`. One dispatch where the region had thousands.
- **backward:** `T = stan::math::var` inside a `nested_rev_autodiff`.
  Every `MUL`, every `EXP` allocates a vari on the nested stack; `grad()`
  walks it through virtual `chain()` calls; the live-ins' adjoints are
  harvested at the end.

Three measured consequences, all in `docs/benchmarks.md`:

1. **The carve estimate has to weigh the register file 4x** ("built twice
   per call and once as vars") and still refuses 13 of the 14 regions it
   can compile. Only `iohmm_reg` wins, and it wins for a reason unrelated
   to compute: its steps copy a 1,500-element state vector, 1.6M elements
   of traffic per gradient, and the registers make those copies vanish.
2. **`bones_model` inverts the same mechanism to 0.05x** — 36 ops behind
   a 4,024-register file, rebuilt as vars 77 times per gradient.
3. **Op collapse is not the win.** `hmm_gaussian` collapses 42,926 ops to
   11 and measures 1.01x. A dispatch is ~5 ns; a var node is not.

So the island machinery today buys data movement, not arithmetic. A
native adjoint changes what it buys.

## Design

### The artifact

`gen_adjoint(const Program& fwd) -> AdjointProgram`, run once at island
compile time, alongside the existing forward program. Both are stored in
the `IslandProg` payload in `g.udata_pool`.

Gradient evaluation becomes, entirely in doubles:

1. run the forward program (already happens in the forward sweep; its
   register file and checkpoints are retained for the backward)
2. zero the adjoint register file
3. seed the output registers' adjoints from the op's incoming adjoint
4. run the adjoint program
5. read the live-ins' adjoints out to the op's input adjoint slots

No vari, no nested stack, no virtual dispatch, no allocation. The
executor's own promise — "the op graph is the AD tape, steady-state
gradient evaluation performs zero allocation" — finally holds one level
down inside the island too.

### The instruction set

Reverse-mode source transformation over `Program::Code`, which is ~35
opcodes and closed. The adjoint program is a `Program` with a few added
instruction forms, so `run_program` stays the single interpreter:

| forward | adjoint |
| --- | --- |
| `ADD d=a+b` | `adj[a] += adj[d]; adj[b] += adj[d]` (pure routing) |
| `SUB d=a-b` | `adj[a] += adj[d]; adj[b] -= adj[d]` |
| `MUL d=a*b` | `adj[a] += adj[d]*val[b]; adj[b] += adj[d]*val[a]` |
| `DIV d=a/b` | `adj[a] += adj[d]/val[b]; adj[b] -= adj[d]*val[d]/val[b]` |
| `EXP d=exp(a)` | `adj[a] += adj[d]*val[d]` (reads the *output*) |
| `LOG d=log(a)` | `adj[a] += adj[d]/val[a]` |
| `MOV/MOVR` | adjoint routing, reversed |
| `CONST/CONSTR` | nothing |
| `GT..NE` | nothing — comparisons already have no derivative |
| `DOT d=a.b` | `adj[a] += adj[d]*val[b]` (ranged axpy), symmetric |
| `LSE_RANGE` | `adj[a+i] += adj[d]*softmax(val[a..])[i]` |
| `SOFTMAX` | the Jacobian contraction the graph kernel already has |
| `LOG_MIX`, `LSE2` | the batched kernels' existing backward formulas |
| densities | one recorder call for the partials, then multiply-accumulate |

New instruction forms needed, all ranged variants of one shape:
`AXPY_ADJ` (`adj[dst] += adj[src] * val[k]`), `ACC_ADJ`
(`adj[dst] += adj[src]`), and their range forms. Everything else reuses
the existing arithmetic opcodes operating on the adjoint file.

**The densities keep the stan-math reuse.** `rvar` — the recorder scalar
— exists precisely to make stan-math compute values *and partials* in
doubles with no tape, and it is already what the scalar density ops use
for their backward. A density adjoint instruction is one recorder call
plus a multiply-accumulate, not a hand-written derivative. The formal
goal of the project (near-total stan-math reuse) survives this pass
intact; nothing here re-derives math that stan-math owns.

### The real compiler problem: value availability

Registers are mutable cells. The backward needs operand and output values
from the forward, and a register overwritten later in the forward has
lost them. This is where the pass earns its keep, and it is *not* full
SSA — it is a minimal checkpoint set:

- Classify each opcode by what its adjoint rule reads: **nothing**
  (`ADD`, `SUB`, `MOV`, `NEG`, `CONST`, all comparisons), **operands**
  (`MUL`, `DIV`, `LOG`, `POW`, `DOT`), or **its own output** (`EXP`,
  `SQRT`, `INV_LOGIT`, `TANH`, `SOFTMAX`).
- For each instruction, a value is needed only if the register it lives
  in is written again before the end of the forward program. Liveness
  over the generated adjoint code decides this exactly.
- Where it is needed, emit a save into a fresh checkpoint register.

This interacts directly with the register aliasing that made `iohmm_reg`
viable in the first place (dead copy-then-modify chains reuse their
base's registers: 1.6M registers down to 94k). **Keep the aliasing; un-
alias only what the adjoint provably reads.** Getting this backwards —
either dropping the aliasing or checkpointing every register — throws
away exactly the property that made islands worth having. Expect the
checkpoint set to be small: recurrences are mostly `ADD`/`MOV`/`MUL`
chains where one operand is a loop-invariant parameter, and a parameter
register is not overwritten.

### Branches

The lowering-emitted islands (parameter-dependent control flow, once item
2 of the roadmap lands) carry `JZ`/`JMP`; loops are already unrolled at
lowering, so there are no back edges. Two properties make the adjoint
easy:

- Comparisons materialize their 0/1 result **into a register**, so the
  predicate is a value the backward can read, not a control decision it
  has to re-derive.
- Generate the adjoint from the **structured** form (`mir_prog.hpp`),
  not the flattened instruction list: mirror each if/else in reverse,
  branching on the same saved predicate register. Only the taken branch's
  adjoint runs — which is exactly the "differentiate the taken branch"
  semantics the var replay has today, and what generated C++ does.

A predicate register is by construction a value the adjoint reads, so it
joins the checkpoint set.

### What it unlocks beyond speed

**The carve estimate flips.** `kVarWeight = 4` exists because the backward
builds the register file as vars. With a native adjoint the backward
costs roughly what the forward costs, so nearly every region where op
collapse wins becomes a net win. The 13 regions the carver refuses today
(`arma11`, `garch11`, `hmm_drive_*`, `hmm_example`, `Mb_model`,
`accel_*`, `losscurve_sislob`, `hier_2pl`, `multi_occupancy`,
`hmm_gaussian`, `bones_model`) re-enter on their merits, and
`bones_model`'s pathology disappears rather than needing to be estimated
around. The estimate stays — it just stops being dominated by one term.

**Propto and target-term absorption become possible.** Islands refuse
propto densities today because term-dropping depends on argument *types*
and the replay's uniform `T` binding cannot reproduce it. A generated
adjoint has no uniform-binding constraint: the generator knows each
argument's activity statically and emits exactly the value terms and
partials that mask implies. That is what lets `garch11`/`arma11` pull
their density calls *into* the island, which is where their remaining gap
lives.

## Expected magnitude — stated before measuring

Replay costs roughly CmdStan's per-node cost (allocation plus a virtual
`chain()`, tens of ns per operation). A native adjoint costs about what
the forward interpreter costs (a few ns per instruction). So the backward
should go from dominating the island to roughly matching the forward.

**The ceiling is parity-plus, not 5x.** CmdStan's generated code is
inlined compiled C++; the adjoint program still pays interpreted dispatch
per instruction. The honest claim is that this converts the sequential
tail (0.59-0.94x) to parity-or-better and removes the "islands only win
on data movement" restriction — it makes op collapse worth what the op
counts always suggested it should be. If the measurement disagrees, the
measurement is right; write down which models moved and by how much, the
way the island table does.

## Verification

The lesson from the island pass is written into its own postmortem: **A/B
every model the pass fires on, not just the target model.** Ten of twelve
were net losses and only the corpus-wide A/B said so.

1. `STANLI_NO_NATIVE_ADJ=1` selects the var replay, so every island model
   has a same-build A/B baseline.
2. **Bitwise** against the replay backward on every model that carves an
   island. To keep that, the generator must emit adjoints in exact reverse
   instruction order and accumulate a given instruction's operand adjoints
   in the order `chain()` does. This is a solved problem in this tree: the
   batched `LOG_MIX`/`LSE2` kernels went through precisely this
   ("accumulated descending to match the reverse sweep") and came out
   bit-identical.
3. **Two-gradient test** for adjoint double-counting. The island work hit
   exactly this: live-in ∩ live-out slots double-counted, off by exactly
   1.0, caught only because a second gradient at the same point differed.
   The checkpoint/aliasing interaction has the same failure shape.
4. `harnesses/ab_corpus.py` over the corpus, then the CmdStan differential
   rig (`tools/verify_sample.py`) on anything that moves.
5. Re-run the carve-estimate table in `docs/benchmarks.md` and replace it.
   The 14-row island table is the artifact this pass is measured against.

## Shape of the work

- `runtime/include/stanli/adjoint.hpp`, `runtime/src/adjoint.cpp`:
  `gen_adjoint`, the opcode→adjoint-rule table, the checkpoint/liveness
  analysis.
- `program.hpp`: ~10 new instruction forms (adjoint accumulate/axpy and
  their ranged variants).
- `runtime/kernels/island.cpp`: backward takes the generated program;
  the var replay stays behind `STANLI_NO_NATIVE_ADJ` as the oracle.
- `island.cpp`: re-tune `kVarWeight` against measurement, not assumption.
- `tests/test_adjoint.cpp`: per-opcode adjoint rules against the var
  replay at fixed points, bitwise.

The interesting work is concentrated in the checkpoint-set/liveness
interaction with register aliasing. Everything else is a table.
