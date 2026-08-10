# Tutorial: one small model through every layer

This is a guided tour of how stanli turns a Stan model into a gradient.
We take one ten-line model and follow it all the way down: the
compiler's tree, the op graph, the two kinds of compiled region, the
place where a value has to be saved before it is destroyed, the
generated backward pass, and the cost model that decides whether any of
this was worth it. Every listing is genuine tool output, and the last
section shows how to reproduce all of it yourself.

If you have not read [how-it-works.md](how-it-works.md), the one
paragraph you need from it is this: stanli does not generate code. A
model becomes a list of operations over flat arrays, the list is
executed forward for the value and backward for the gradient, and
because the list never changes, it doubles as the autodiff tape. The
question this tutorial answers is what happens to the parts of a model
that resist becoming a nice flat list.

A few words used throughout, defined once:

| word | meaning |
| --- | --- |
| slot | one value in the graph: a scalar, vector, or matrix, living at a fixed place in one big array (the arena) |
| op | one step of the graph: reads some slots, writes a slot |
| kernel | the C++ that implements an op: a `forward` function and a `backward` function |
| register | one *number* in an island's private array; a vector is a run of consecutive registers |
| adjoint | the running derivative of the final answer with respect to some value; the backward pass's currency |
| scratch | per-op working memory, sized at load time, where a forward stashes what its backward will need |
| pool | an island's table of constants, baked in at load time |

## The model

```stan
data {
  real y;
}
parameters {
  real theta;
  real<lower=0> sigma;
}
model {
  // A branch that depends on a parameter.
  real m;
  if (theta > 0) {
    m = 2 * theta;
  } else {
    m = -theta;
  }
  // A vector that depends on nothing but numbers.
  vector[2] w = [0.8, 0.2]';
  // A recurrence: each step reads what the previous step wrote.
  vector[2] s = [m, m * 0.5]';
  real lp = 0;
  for (t in 1:10) {
    real a = dot_product(s, w);
    s[1] = tanh(a);
    s[2] = s[2] * 0.9;
    lp += normal_lpdf(y | a, sigma) + lgamma(a + 2);
  }
  target += lp;
}
```

Each line was chosen to force a different mechanism to appear:

- `if (theta > 0)` cannot become graph ops at all, because a fixed
  list cannot contain a decision. It becomes a compiled region with
  real jump instructions.
- `w` depends only on numbers, so it is computed once at load and
  never exists as ops.
- The loop is a recurrence, which nothing can vectorize, so it
  becomes the other kind of compiled region.
- `dot_product` is an operation whose derivative needs its *inputs'
  values*, and those inputs get overwritten. Something will have to
  save them.
- `normal_lpdf` and `lgamma` show the two ways math reaches the Stan
  library from inside a region.

## Layer 1: the compiler's tree

`stanc --debug-transformed-mir` prints the model as a typed tree (the
MIR). Two excerpts, trimmed:

```
(IfElse ((FunApp (StanLib Greater__ ...)
           (Var theta) ... (adlevel AutoDiffable) ...
           (Lit Int 0)  ... (adlevel DataOnly) ...)) ...)

(FunApp (StanLib normal_lpdf (FnLpdf false) AoS) ((Var y) ...))
```

Two annotations decide everything that follows. `adlevel
AutoDiffable` on `theta` is the compiler recording that a parameter
reaches this expression. When lowering sees that marker on an `if`
condition, it knows the branch cannot be picked at load time -- a
condition on data would be decided once, right there, and only the
taken arm would be lowered. And `(FnLpdf false)` says this density
call keeps all its constant terms, because it was written `lp +=
normal_lpdf(...)` rather than `y ~ normal(...)`. The dropped-constant
form of `~` depends on which arguments are parameters, which is a
distinction the machinery below deliberately does not model -- writing
the explicit form is what makes the density expressible there.

## Layer 2: the op graph

Lowering walks the tree and emits ops. `dump_ops` prints the result:

```
slots=126 ops=10 result=125
  0 CONSTRAIN_LOWER  out=s4    in=s1(P),s2
  1 ISLAND           out=s5    in=s0(P)
  2 INDEX            out=s6    in=s5
  3 MUL              out=s9    in=s6,s8
  4 CONCAT2          out=s10   in=s6,s9
  5 ISLAND           out=s124  in=s10,s4
  6 INDEX            out=s112  in=s124
  7 INDEX            out=s122  in=s124
  8 ADD              out=s123  in=s112,s122
  9 ADD_N            out=s125  in=s123,s3
```

Ten ops for the whole model. Reading top to bottom:

- **Op 0** turns the unconstrained `sigma` (slot 1; `P` marks a
  parameter slot) into the positive `sigma` the model sees, and its
  second output is the log-jacobian of that transform, which joins the
  target.
- **Op 1** is the entire `if`/`else`, already one op: `theta` in, `m`
  out. How it got that way is layer 3.
- **Ops 2-4** build `s = [m, m * 0.5]'` as ordinary graph ops: read
  `m`, multiply by the constant 0.5, concatenate.
- **Op 5** is the entire loop. Run the same command with
  `STANLI_NO_ISLAND=1` and this model is 115 ops -- ten iterations of
  DOT, TANHV, MUL, NORMAL_LPDF, LGAMMA, and ADDs. The carver collapsed
  108 of them into this one op. That is layer 4.
- **Ops 6-7** unpack the island's two outputs (the final `lp` pieces)
  into the slots the rest of the graph reads, and **ops 8-9** sum them
  with the jacobian term into the final log density.

And `w` is nowhere. `[0.8, 0.2]'` depends on no parameter, so during
lowering the MIR interpreter -- the slow, name-driven evaluator that
handles anything -- computed it once, and the values became constants.
You will see them again below, inside the island.

The gradient of this graph is the ordinary story: run ops 0-9 forward,
then run their kernels' backwards in reverse order, 9 down to 0, each
one reading its output's adjoint and adding to its inputs' adjoints.
Every op here knows how to do that. The interesting question is what
"run the backward" means for ops 1 and 5, which are not one operation
but a compiled region of them.

## Layer 3: the branch, compiled with real jumps

`dump_islands` prints what is inside op 1:

```
11 instrs, 8 regs, adjoint 0 instrs, native_adj=0
live-ins: slot0 -> r2        live-outs: r1

   0  CONST  r0 <- pool[0] (=0)
   1  CONST  r1 <- pool[1] (=nan)
   2  CONST  r3 <- pool[0] (=0)
   3  GT     r4 <- r2, r3
   4  JZ     if r4 == 0 goto 9
   5  CONST  r5 <- pool[2] (=2)
   6  MUL    r6 <- r5, r2
   7  MOV    r1 <- r6
   8  JMP    goto 11
   9  NEG    r7 <- r2
  10  MOV    r1 <- r7
```

This is a tiny program for a tiny machine: numbered registers, a
constant pool, and an instruction list executed top to bottom. "Live-in
`slot0 -> r2`" means: before running, copy the value of graph slot 0
(`theta`) into register 2. "Live-out `r1`" means: after running, the
op's output is whatever register 1 holds.

Now read it as the `if` statement. Register 1 is `m`, and it starts as
NaN because Stan defines an unassigned local variable to be NaN --
instruction 1 is that rule, made explicit. Instruction 3 compares
`theta` with zero and writes the answer *as a number*, 0 or 1, into
register 4: the predicate is a value, not a control decision.
Instruction 4 reads that value and jumps to the else-arm when it is
zero. Both arms end by writing register 1, so whichever arm ran, the
live-out is in the same place. Instruction 8 jumps past the else-arm --
`goto 11` is one past the end, meaning "done".

The header line says `adjoint 0 instrs, native_adj=0`: this program's
backward is **not** generated. Running an if/else backward requires
knowing which instructions belong to which arm, and this flat list has
already forgotten -- the arms are just address ranges now. So the
backward here is the older, slower mechanism, the *replay*: run the
same 11 instructions again, but on stan-math's autodiff scalar instead
of plain doubles, let it build its little tape (about four nodes), and
read `d(m)/d(theta)` off the tape. Because the comparison at
instruction 3 reads *values*, the replay takes the same arm the
forward took, so the derivative is the derivative of the branch that
actually ran -- which is exactly what CmdStan's generated C++ computes
for the same statement. Branches on parameters are rare in real
models, and slow is acceptable here; what matters is that the model
compiles and the answer is right.

## Layer 4: the loop, compiled into the same machine

Op 5 is the same kind of program, without jumps and much longer: 123
instructions, ten near-identical blocks. Here is the header and the
first block, one loop iteration:

```
123 instrs, 118 regs, 10 calls, adjoint 112 instrs, native_adj=1
live-ins: slot10 -> r0[len 2]   (s)
          slot4  -> r10         (sigma)
live-outs: r89 r97

   0  CONSTR   r2..r3 <- pool[0..] (=0.8,0.2)
   1  MOVR     r98..r99 <- r0..r1
   2  DOT      r4 <- dot(r0.., r2.., len 2)
   3  TANH     r5 <- r4
   4  MOV      r0 <- r5
   5  MOV      r6 <- r1
   6  CONST    r7 <- pool[2] (=0.9)
   7  MUL      r8 <- r6, r7
   8  MOV      r1 <- r8
   9  CONST    r9 <- pool[3] (=0.7)
  10  DENSITY  r11 <- normal_lpdf(r9, r4, r10)
  11  CONST    r12 <- pool[4] (=2)
  12  ADD      r13 <- r4, r12
  13  CALL     OP_LGAMMA(r13[len 1]) -> r14[len 1], no scratch
  14  ADD      r15 <- r11, r14
  15  CONST    r16 <- pool[5] (=0)
  16  ADD      r17 <- r16, r15
```

Line up the Stan source against the instructions:

- **Instruction 0** is `w`. The MIR interpreter folded it at load,
  and here it is, two constants from the pool. `y` (0.7, absorbed from
  the data) and the literals 0.9, 2, and 0 arrive the same way at
  instructions 9, 6, 11, and 15.
- **Instruction 2** is `a = dot_product(s, w)`: the state `s` lives in
  registers 0-1, `w` in 2-3.
- **Instructions 3-4** are `s[1] = tanh(a)` -- and notice there is no
  copy. At the graph level, assigning one element of a vector makes a
  fresh copy of the whole vector, every time, because some *other*
  reader might still need the old one. Inside the island the compiler
  proved nothing else reads the old state, so all ten per-iteration
  copies of `s` collapsed onto the single register pair 0-1, and the
  assignment is just an overwrite. **Registers are reused; that fact
  drives everything about the backward.**
- **Instructions 5-8** are `s[2] = s[2] * 0.9`, the same way.
- **Instruction 10** is the density. `DENSITY` is one instruction that
  can be any of the 27 scalar continuous densities; which one rides in
  the instruction, and the three operands are registers holding `y`,
  `a`, and `sigma`. Running it forward calls the ordinary
  `stan::math::normal_lpdf` on three plain doubles. Value only;
  nothing is recorded.
- **Instruction 13** is `lgamma`, and the machine has no lgamma
  instruction. Instead of one, it has `CALL`: run the graph's own
  `OP_LGAMMA` kernel -- the identical C++ the graph executor would
  have dispatched -- over these registers. Any scalar operation the
  graph knows, the island can now say. A kernel that stashes partials
  would get a scratch range carved out of this same register file;
  lgamma's does not need one.
- **Instructions 14-16** accumulate `lp`.

That leaves instruction 1, which is the subtle one.

### The checkpoint: saving a value before it is destroyed

The backward pass of `dot_product` must compute `d(a)/d(w_i) = s_i`:
it needs the *values* of `s` at the moment the dot product ran. But
registers 0-1 are overwritten at instructions 4 and 8, and again in
every one of the nine following iterations. By the time any backward
pass runs, the values instruction 2 read are long gone.

The adjoint generator finds exactly this situation. When it compiles
the backward (next layer), it classifies every instruction by what its
derivative rule needs -- nothing for additions and copies, its
operands' values for multiplies and dots, its own output's value for
`exp` and `tanh` -- and then checks, for each needed value, whether
the register holding it is ever written again afterward. Almost every
value survives: `r4` is written once and read later, fine; the
constants are never touched; `sigma` sits in `r10` untouched. The one
failure in this whole program is the state pair, ten times over. So
the generator inserts instruction 1: copy registers 0-1 into fresh
registers 98-99 before anything overwrites them. Each iteration gets
its own two-register save (`r98..r99`, then `r100..r101`, and so on).
Twenty registers of insurance, and nothing else in 123 instructions
needed any.

This is the general shape of the trade reverse-mode autodiff always
makes: remember enough of the forward pass to run the chain rule
backward. CmdStan remembers by heap-allocating a tape node per
operation, every evaluation. Here, remembering is two `MOVR`
instructions per iteration, decided once at load, into memory that
already exists.

## Layer 5: the backward, as a second program

The header said `adjoint 112 instrs, native_adj=1`: alongside the
forward program, the compiler generated its derivative -- the forward
list read backward, each instruction replaced by its chain-rule step.
It runs over a second array, the *adjoint file*, one cell per
register: cell k holds "the derivative of the final answer with
respect to the value currently in register k."

The first lines (they mirror the *last* forward instructions, since
the order is reversed; `dst`, `a`, `b`, `c` name adjoint cells and
`va`, `vb`, `vd` name value registers):

```
   0  ADD      dst=97 a=94 b=96
   1  CALL     a=9
   2  ADD      dst=95 a=90 b=12
   3  DENSITY  dst=94 a=9 b=90 c=10 | va=9 vb=90 vc=10
   4  MUL      dst=93 a=1 b=7      | va=92 vb=7
   5  MOV      dst=0 a=91
   6  TANH     dst=91 a=90         | va=90
   7  DOT      dst=90 a=0 b=2      | va=116 vb=2
```

Each step does the same three things: take the adjoint waiting in its
output's cell, clear that cell (the register is about to mean an older
value as we keep walking backward, and an older value has its own
derivative), and add contributions to its operands' cells. Walk them:

- **Step 0**, the adjoint of `lp += lpdf + lgamma`: addition
  contributes its adjoint to both operands unchanged. No values
  needed. Pure routing.
- **Step 1** is the `CALL`'s backward: the `OP_LGAMMA` *kernel's own
  backward function*, run over the island's registers. The graph and
  the island share one derivative implementation because they share
  the kernel.
- **Step 3** is the density's backward, and this is where most of a
  real model's mathematics lives. It wraps the three argument values
  in a recording scalar and calls the *same* `stan::math::normal_lpdf`
  template again; stan-math's own machinery computes the value and all
  three partial derivatives in plain doubles and deposits the partials
  into a four-double array on the C stack. Then three multiply-adds
  spread them: `adj(sigma) += t * d/dsigma`, and so on. Nothing in
  stanli knows the derivative of a normal density; the Stan library
  computed it, without building any tape.
- **Step 4**, `s[2] * 0.9`: the multiply rule needs its operands'
  values, and reads `vb=7` -- the register still holding the constant
  0.9, never overwritten, so no checkpoint was needed.
- **Step 6**, `tanh`: needs its input's value (`va=90`, safe) to
  recompute the slope.
- **Step 7** is the payoff. The dot product's rule needs the state
  values, and its value operand is `va=116` -- not register 0. That is
  the checkpoint from the *tenth* iteration's save, being consumed. On
  the walk back through iteration nine it will read `va=114`, and so
  on down to `va=98`. The insurance taken out in layer 4 is claimed
  here, ten times.

The wiring around this program is short. The island op's scratch slice
*is* its register file, so the values and checkpoints the forward left
behind are simply still there when the backward runs. The backward
zeroes the adjoint file, seeds the cells of the two live-out registers
from the adjoints the graph delivered (via the extraction INDEX ops),
runs the 112 instructions, and finally adds the cells of registers
0-1 and 10 into the arena adjoints of `s` and `sigma`. From there the
ordinary graph backward continues: through `CONCAT2` and `MUL` to
`m`'s adjoint, then into the branch island's replay, and out the other
side as `d/d(theta)`. One number's whole journey, across every
mechanism in the runtime.

## Layer 6: the estimate says no

A twist: on this toy model, the default build does not actually carve
the loop. The carver *compiled* the island, generated its backward,
and then priced it against the ops it would replace:

```
island? ops=108 graph=658 island=669
```

The graph side counts the data the 108 ops move plus a fixed per-op
dispatch tax. The island side counts its registers (written by the
forward, read by the backward), both instruction lists, and -- because
a `CALL` runs the graph's own kernel with the graph's own overhead --
the very same per-op tax for each of the ten CALLs. Absorbing a CALL
buys continuity of the region, never speed, and the estimate prices it
that way. 658 against 669: at ten iterations of this shape, the island
is a wash, so the ops stay and correctness is identical either way.
Every listing above was produced with `STANLI_ISLAND_ALWAYS=1`, the
switch whose entire purpose is asking "what would you have built, and
why did you decline?" Scale the same shape up -- more steps, a wider
state, HMM emissions -- and the ratio flips; the corpus table in
[benchmarks.md](benchmarks.md) is this same estimate run against
every region in the corpus.

## The receipts

Three configurations, same model, same point:

```
default          OK -10.372086118363603 11.984608360112961 -5.5685067343452275
islands forced   OK -10.372086118363603 11.984608360112961 -5.5685067343452275
islands off      OK -10.372086118363603 11.984608360112961 -5.5685067343452275
```

Log density, `d/d(theta)`, `d/d(sigma)` -- identical to the last
digit whether the loop ran as 108 graph ops or as one island under the
generated backward. That invariant is what every layer above is built
to preserve, and the corpus replay holds all 119 models to it against
recorded CmdStan values on every push.

## Reproduce it

From a built checkout (see [hacking.md](hacking.md)), save the model
above as `toy.stan` and its data as `toy.json`:

```json
{"y": 0.7}
```

Then:

```
./deps/stanc3/stanc --debug-transformed-mir toy.stan > toy.sexp

build-rel/dump_ops     toy.sexp toy.json            # layer 2
STANLI_NO_ISLAND=1 \
build-rel/dump_ops     toy.sexp toy.json -1         # ...without islands
STANLI_ISLAND_ALWAYS=1 \
build-rel/dump_islands toy.sexp toy.json            # layers 3-5
STANLI_DEBUG_ISLAND=1 \
build-rel/stanli_check toy.stan toy.json            # layer 6, on stderr

build-rel/stanli_check toy.stan toy.json            # the receipts
STANLI_ISLAND_ALWAYS=1 build-rel/stanli_check toy.stan toy.json
STANLI_NO_ISLAND=1     build-rel/stanli_check toy.stan toy.json
```

Register numbers may shift as the compiler evolves; the shapes --
the jumps, the collapsed state pair, the checkpoints, the DENSITY and
CALL instructions, the mirrored backward -- are the load-time
artifacts this tutorial is about.
