# One vocabulary: the register machine calls the graph's kernels

**Question this answers:** can the adjoint machine, the graph executor,
and the MIR interpreter be unified -- or at least made to speak the same
ops?

## The census

Measured from the macro lists, not estimated:

| engine | vocabulary |
| --- | ---: |
| graph executor (kernels) | 294 opcodes |
| register machine (islands, ODE RHS, parameter branches) | 50 |
| MIR→Program front end (mir_prog.hpp) | 30 names + densities |

The 244-op gap is not exotic: 75 scalar cdfs (every truncation), 37
scalar unaries, 38 ordered densities, 15 int cdfs, 4 int densities, and
75 plain ops -- including `POW`, which the carver's own test uses as the
op that *splits* a region. One out-of-vocabulary op in the middle of
3,000 compilable ones ends the run.

## Why full unification is the wrong goal

The three engines are the same semantics at three binding times, and
each exists because of what it binds late:

- The **MIR interpreter** binds *names* at evaluation time. It is the
  semantic fallback for anything stanc emits -- dynamic shapes, RNG,
  functions with no lowering. Its slowness is the price of its totality,
  and it is the reference the other two degrade to.
- The **graph executor** binds *shapes and order* at compile time and
  values at evaluation time. Vector-granular, tape = op list.
- The **register machine** additionally binds *placement* at compile
  time: no slots, no context assembly, cells addressed by constant.
  Scalar-granular, and the only one with control flow, because the other
  two are straight-line by construction.

Collapsing them means giving one of these up. What CAN be unified is the
**vocabulary and the derivative rules**, because the kernel interface is
already the union point:

    forward(ctx):  read ctx.in, write ctx.out, stash partials in scratch
    backward(ctx): read scratch (+ values), consume out_adj,
                   accumulate ctx.in_adj

That is precisely one generated-adjoint step. The graph's backward has
been "native" all along, at op granularity -- constfold already treats
kernels as "the only definition of what an op computes", and this
extends that to the register machine.

## The design: `CALL`

One instruction. Its payload (a `Program::Call` in a side pool, like
idata) names a graph opcode, the register ranges standing in for its
slots, its copied idata, and a scratch range **allocated inside the
island's register file** -- so the partials the forward stashes are
retained for the backward exactly like every other register.

- **forward:** assemble a `KernelCtx` over the register file, call the
  kernel's `forward`. Double-only: `run_program<var>` throws on CALL,
  and the carver only keeps a CALL-bearing island when the generated
  adjoint exists, so the var replay can never meet one.
- **adjoint:** assemble the backward ctx -- `in_adj`/`out_adj` into the
  adjoint file at the same ranges, values at the *checkpointed*
  registers -- call the kernel's `backward`, then zero the output's
  adjoint cells (cell reuse semantics, as every other rule).
- **checkpoints:** every CALL input and output range is conservatively
  value-saved if overwritten later, because some kernel backwards
  re-read values (`backward_ignores_input_values` is a whitelist, not a
  guarantee). CALL ranges are excluded from adjoint-cell aliasing so a
  range stays a range.
- **oracle:** a CALL runs the *graph's own kernel*, so the arbiter is
  the passes-off graph -- the CmdStan-verified baseline -- which is the
  stronger oracle anyway. The var replay remains the oracle for
  everything that is not a CALL.

### Scope

**Built:** the carver emits CALL for scalar-out ops -- kernel exists,
no `out2`, no `udata`, propto off, not meta
(`ISLAND`/`ODE`/`PRINT`/`REJECT`). That is the cdfs, the unaries, the
lpmfs, `POW`, `LOG_DIFF_EXP` -- the entire scalar tail. Out-of-vocab
ops stop ending runs.

**Deliberately not planned** (user decision 2026-08-09: the axis is
simplicity and readability, not squeezing parameter conditionals or
ODEs, which are rare shapes; sequential models already crossed parity):

- Vector/matrix-out CALLs. Would need a boundary policy so whole
  vectorized models are not compiled just to be refused; the models it
  would help are at 0.97-1.00x already.
- `mir_prog.hpp` emitting CALL for parameter-dependent branches, and
  the structured branch adjoint it depends on. Branchy regions keep
  the var replay; they are rare and slow-is-fine. What must NOT
  regress is that they *compile* -- the shared density table is what
  keeps the vocabulary cliff closed there.
- Propto absorption through the variant byte. Real, measurable, and
  not worth its A/B until a model asks for it.

### What stays three things

The MIR interpreter keeps its role and already shares the pieces that
are shareable: `program_density` (this branch) and
`STANLI_SCALAR_UNARY_LIST`. Its vocabulary is name-driven and wider than
both machines; where it lacks something it fails loudly at compile time,
which is its contract.

## Estimate interaction

CALL scratch registers are working memory the graph op also had (for
free, in the estimate's eyes), so they are excluded from the
`kRegWeight` term and counted at weight 1. A CALL's own cost is
otherwise the same kernel the graph ran; its contribution to both sides
of the estimate roughly cancels, which is correct -- the point of
absorbing it is not making it faster, it is not ending the run.
