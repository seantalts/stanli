# Tutorial: three small models through every layer

This is a guided tour of how stanli turns a Stan model into a gradient,
and it is meant to teach the whole runtime. Three tiny models, in
order of how often their shape occurs in practice:

1. **A linear regression** -- the normal path, the one nearly every
   real model takes, and the reason stanli is usually faster than a
   compiled model.
2. **A branch on a parameter** -- the one thing a fixed list of
   operations cannot express, and the machinery that catches it.
3. **A recurrence** -- a loop that nothing can vectorize, and the
   compiled region with a generated derivative that makes it fast
   anyway.

By the end you will have seen every mechanism in the runtime do its
job on real output: every listing below is what the tools actually
print, and the last section shows how to reproduce all of it.

If you have not read [how-it-works.md](how-it-works.md), the one
paragraph you need from it is this: stanli does not generate machine
code. A model becomes a list of operations over flat arrays, the list
is executed forward for the value and backward for the gradient, and
because the list never changes, it doubles as the autodiff tape. This
tutorial is that paragraph, made concrete three times.

A few words used throughout, defined once:

| word | meaning |
| --- | --- |
| slot | one value in the graph: a scalar, vector, or matrix, living at a fixed place in one big array (the arena) |
| op | one step of the graph: reads some slots, writes a slot |
| kernel | the C++ that implements an op: a `forward` function and a `backward` function |
| adjoint | the running derivative of the final answer with respect to some value; the backward pass's currency |
| scratch | per-op working memory, sized at load time, where a forward stashes what its backward will need |
| register | one *number* in an island's private array; a vector is a run of consecutive registers |
| pool | an island's table of constants, baked in at load time |

## Part 1: the normal path

### The model

```stan
data {
  int<lower=0> N;
  vector[N] x;
  vector[N] y;
}
parameters {
  real alpha;
  real beta;
  real<lower=0> sigma;
}
model {
  y ~ normal(alpha + beta * x, sigma);
  alpha ~ normal(0, 1);
  beta ~ normal(0, 1);
  sigma ~ normal(0, 1);
}
```

A linear regression with priors. No branches, no loops, no tricks:
this is the shape of most models people actually run, and everything
about how stanli handles it generalizes to GLMs and hierarchical
models directly.

### Layer 1: the compiler's tree

stanli does not parse Stan itself. The official compiler, stanc3, is
linked in and asked for its typed intermediate representation, the
MIR. `stanc --debug-transformed-mir` prints it; here is the sampling
statement, trimmed:

```
(FunApp (StanLib normal_lpdf (FnLpdf true) AoS)
  ((Var y)   ... (adlevel DataOnly) ...
   (FunApp (StanLib Plus ...))  ... (adlevel AutoDiffable) ...))
```

Two annotations on this tree decide everything downstream.

**`adlevel`** is the compiler's taint analysis: `AutoDiffable` means
"a parameter reaches this expression", `DataOnly` means it is fixed
once the data is read. Lowering trusts these labels completely. A
`DataOnly` subtree never becomes ops -- it is evaluated once, at load,
and its result becomes a constant. An `AutoDiffable` subtree is the
part the gradient has to flow through, so it must become something
differentiable.

**`(FnLpdf true)`** says this density was written with `~`, so its
constant terms are dropped -- the `lup` in the internal name CmdStan
uses, "log unnormalized probability". Which terms get dropped depends
on which arguments are parameters, and that exact information is the
`adlevel` of each argument. Hold that thought for one layer.

### Layer 2: the op graph

Lowering walks the tree and emits ops. `dump_ops` prints the result
(run with the data file below, N = 5):

```
slots=16 ops=8 result=15
    0 CONSTRAIN_LOWER v=00 out=s5(len1) in=s2(l1,P),s3(l1)
    1 MUL        v=00 out=s8(len5) in=s1(l1,P),s7(l5)
    2 ADD        v=00 out=s9(len5) in=s0(l1,P),s8(l5)
    3 NORMAL_LPDF v=86 out=s10(len1) in=s6(l5),s9(l5),s5(l1)
    4 NORMAL_LPDF v=81 out=s12(len1) in=s0(l1,P),s3(l1),s11(l1)
    5 NORMAL_LPDF v=81 out=s13(len1) in=s1(l1,P),s3(l1),s11(l1)
    6 NORMAL_LPDF v=81 out=s14(len1) in=s5(l1),s3(l1),s11(l1)
    7 ADD_N      v=00 out=s15(len1) in=s10(l1),s12(l1),s13(l1),s14(l1),s4(l1)
```

Eight ops for the whole model. Read the columns first: each op writes
one output slot (`out=s10(len1)`: slot 10, one element) and reads
input slots; `P` marks a slot that is part of the unconstrained
parameter vector; `l5` is a length. Slots are not variables -- they
are offsets into one flat value array, the *arena*, assigned at load
time and never reassigned. Reverse mode has a second, compact arena:
parameters first, then only slots surviving ops write (plus an otherwise
constant result). Data and slots left behind by graph rewrites need no
adjoint cell and are not cleared on every gradient.

Now read the ops as the model:

- **Op 0** turns the unconstrained `sigma` (slot 2) into the positive
  `sigma` the model sees (slot 5). Its second output, slot 4, is the
  log-jacobian of that transform, which joins the target at the end.
  This is how every constrained parameter enters a model.
- **Ops 1-2** are `alpha + beta * x`. One multiply of a scalar by a
  length-5 vector, one broadcast add. The data vector `x` is slot 7:
  no op produces it, because data slots are just filled once at load.
- **Op 3** is the sampling statement. One op, whatever N is: the
  observed `y` (slot 6), the predicted mean vector (slot 9), and
  `sigma` go in; one number, the summed log density, comes out.
- **Ops 4-6** are the three priors, the same density with scalar
  arguments.
- **Op 7** sums the four density terms and the jacobian into the
  final log density, slot 15.

The count is the point. This graph is 8 ops at N = 5 and still 8 ops
at N = 5,000,000; only the slot lengths grow. CmdStan's generated
C++ walks the same statement by building a heap-allocated autodiff
tape node per operation *per evaluation*, so its cost per gradient
grows with N in both time and allocation. stanli's per-evaluation
cost is eight kernel dispatches over preallocated arrays. This is
why vectorized-statement models -- most of the corpus -- run 1.5-6x
faster than CmdStan's compiled code
([benchmarks.md](benchmarks.md)).

Then there is the `v=` column, the *variant* byte, worth decoding
once because it is the `adlevel` story landing. For a density op,
bits 0-5 mark which arguments are autodiffable and bit 7 marks the
dropped-constants form. Op 3 has `v=86` (hex): propto, arguments 1
and 2 (the mean and `sigma`) active, argument 0 (the data `y`)
not. Ops 4-6 have `v=81`: propto, only argument 0 active -- in a
prior, the "observation" is the parameter. The kernel uses this to
instantiate exactly the data/parameter combination CmdStan's
generated code would have used, which is what makes the dropped
constants, and therefore the log density, match CmdStan bitwise.

### Layer 2½: write it as a loop, get the same graph

Real models often say the same thing element by element:

```stan
  for (n in 1:N) {
    y[n] ~ normal(alpha + beta * x[n], sigma);
  }
```

Swap that into the model and dump again:

```
slots=41 ops=8 result=40
    0 CONSTRAIN_LOWER v=00 out=s5(len1) in=s2(l1,P),s3(l1)
    1 MUL        v=00 out=s36(len5) in=s1(l1,P),s35(l5)
    2 ADD        v=00 out=s37(len5) in=s0(l1,P),s36(l5)
    3 NORMAL_LPDF v=86 out=s39(len1) in=s38(l5),s37(l5),s5(l1)
    ...
```

The same eight ops. Because N is known once the data is loaded,
lowering unrolls the loop -- five scalar densities, five scalar
multiplies -- and then the optimization passes roll it back up:
recognize five ops that differ only by index, prove the iterations
independent, and fuse them into the vector ops above. (The higher
slot count is the leftover numbering from the unrolled
intermediates; nothing computes them anymore.) The unroll-then-reroll
round trip is what makes the loop form and the vectorized form cost
the same, and it is why hierarchical models written with the indexing
idiom `y[n] ~ normal(mu[county[n]], sigma)` still lower to a handful
of vector ops.

What survives all passes is the graph above, and that graph is
*final*: no decision remains inside it. Every remaining question --
which op, which slots, which lengths, which variant -- was answered
at load time.

### Layer 3: forward, or where the math actually lives

At load, the executor walks the op list once and binds each op a
*kernel context*: raw pointers into the arenas for each input and
output, plus a slice of a third preallocated array, *scratch*.
Evaluating the model is then a for-loop: call each op's `forward`
through a function-pointer table, 0 through 7.

Most kernels' forwards are what you would write yourself --
`ADD_N` sums, `MUL` multiplies. The density kernel is where it gets
interesting, because a density forward has a second job: **stash what
the backward will need.** `NORMAL_LPDF`'s forward does not just
compute the summed log density; it also writes, into its scratch
slice, the partial derivative of the result with respect to every
element of every active argument -- for op 3, five partials for the
mean vector and one accumulated partial for `sigma`.

It gets those partials from the Stan library, not from code stanli
wrote. The kernel binds each active argument as a small recording
scalar and calls the very same `stan::math::normal_lpdf` template
that CmdStan calls; stan-math's operands-and-partials machinery
computes the value and the per-argument partials in plain doubles,
and the recorder's job is only to aim them at the scratch slice.
Seventy-plus densities get their derivatives this way, each one the
Stan library's own math, none of it duplicated in stanli.

### Layer 4: backward, or the list read in reverse

There is no tape, because the op list *is* the tape: it was fixed at
load, so nothing needs to be recorded per evaluation. The gradient
is: zero the adjoint arena, set the result slot's adjoint to 1, and
call each op's `backward` in reverse order, 7 down to 0.

Each backward reads its output slot's adjoint and pushes
contributions onto its inputs' adjoints:

- **Op 7** (`ADD_N`): addition routes its adjoint to every operand
  unchanged. The four density slots and the jacobian slot now hold
  adjoint 1.
- **Ops 6-3** (the densities): multiply each partial stashed in
  scratch by the output adjoint and add it into the argument's
  adjoint slot. Op 3's backward adds five numbers into the mean
  vector's adjoints and one into `sigma`'s; the data argument gets
  nothing, which the variant byte decided at load.
- **Ops 2-1**: the broadcast add and scalar-times-vector rules,
  contracting vector adjoints back onto `alpha` and `beta`.
- **Op 0**: chain the constrained `sigma` adjoint through the
  transform's slope, add the jacobian term's adjoint, and write the
  result to unconstrained slot 2's adjoint.

After op 0, the adjoints of slots 0-2 are the gradient, sitting in
declaration order in a flat array the sampler reads directly. No
allocation happened in either direction; the whole evaluation
touched three preallocated arrays through pointers bound once.

### The receipts

`stanli_check` loads the model, evaluates once at a fixed point, and
prints the log density and gradient (after a `WA` line checking the
write_array path, not our concern here):

```
OK -6.1763422914570461 -0.46642082744804991 12.214027581601695 7.5052230767581278
```

That is lp, then d/d`alpha`, d/d`beta`, d/d`sigma`. The same numbers
CmdStan's compiled model produces at the same point -- and not
loosely: the corpus gate replays 119 posteriordb models against
recorded CmdStan values on every push, dozens of them bitwise
identical and the worst within ~3e-12 relative.

One more machine deserves a sentence before we leave the normal
path. Everything in a Stan program that is *not* the log density --
transformed data, generated quantities, `print` statements -- runs on
a plain tree-walking interpreter over the MIR, with names and Stan
semantics, no lowering at all. It also computes any `DataOnly`
expression the graph needs as a constant (you will see it fold a
vector in Part 3). It is a few hundred lines, it handles everything,
and it is slow -- which is fine, because nothing it runs is inside
the sampler's hot loop. Whenever the fast path below cannot express
something, the answer is always "the interpreter still can", which
is what lets the fast path stay small.

## Part 2: a branch on a parameter

### The model

```stan
data {
  real y;
}
parameters {
  real theta;
}
model {
  real m;
  if (theta > 0) {
    m = 2 * theta;
  } else {
    m = -theta;
  }
  y ~ normal(m, 1);
}
```

One decision, one density. The point of this example is the
decision; the density is there so the branch has a downstream
consumer.

### Why the graph cannot say this

In the MIR, the condition carries the fatal annotation:

```
(IfElse ((FunApp (StanLib Greater__ ...)
           (Var theta)  ... (adlevel AutoDiffable) ...
           (Lit Int 0)  ... (adlevel DataOnly) ...)) ...)
```

`adlevel AutoDiffable` on an `if` condition means the branch cannot
be resolved at load time. Had the condition been data, lowering
would simply have evaluated it once and lowered only the taken arm
-- a condition on data is free. But this one changes with `theta`,
per leapfrog step, and Part 1 just told you the graph's defining
property: *no decision remains inside it*. A fixed list of ops
cannot contain an `if`.

stanli's answer is to keep the list fixed and put the decision
inside one op:

```
slots=6 ops=3 result=5
    0 ISLAND     v=00 out=s1(len1) in=s0(l1,P)
    1 INDEX      v=00 out=s2(len1) in=s1(l1) idata=[0]
    2 NORMAL_LPDF v=82 out=s5(len1) in=s3(l1),s2(l1),s4(l1)
```

Op 0 is the entire if/else: `theta` in, `m` out. Op 1 unpacks the
island's output vector into the scalar slot the density reads, and
op 2 is Part 1's density op again (`v=82`: propto, only the mean
argument active). Downstream of the island, everything is the normal
path: the density does not know its mean came out of a branch.

### Inside the island: a program with real jumps

The op contains a program for a second, smaller machine.
`dump_islands` prints it:

```
== island 0 (graph op 0): 11 instrs, 8 regs, 0 calls, adjoint 0 instrs, native_adj=0
  live-ins: slot0->r2[len 1]
  live-outs (out_regs): r1

  FORWARD:
    0  CONST     r0 <- pool[0] (=0)
    1  CONST     r1 <- pool[1] (=nan)
    2  CONST     r3 <- pool[0] (=0)
    3  GT        r4 <- r2, r3
    4  JZ        if r4 == 0 goto 9
    5  CONST     r5 <- pool[2] (=2)
    6  MUL       r6 <- r5, r2
    7  MOV       r1 <- r6
    8  JMP       goto 11
    9  NEG       r7 <- r2
   10  MOV       r1 <- r7
```

Numbered registers, a constant pool, an instruction list executed
top to bottom -- and, unlike the graph, *jump instructions*.
"Live-in `slot0 -> r2`" means: before running, copy graph slot 0
(`theta`) into register 2. "Live-out `r1`": after running, the op's
output is whatever register 1 holds.

Read it as the if statement. Register 1 is `m`, and it starts as NaN
because Stan defines an unassigned local variable to be NaN --
instruction 1 is that rule, made explicit. Instruction 3 compares
`theta` with zero and writes the answer *as a number*, 0 or 1, into
register 4. Instruction 4 turns that number back into control: jump
to the else-arm when it is zero. Both arms end by writing register
1, so whichever arm ran, the result is in the agreed place, and
instruction 8 jumps past the else-arm ("goto 11" is one past the
end, meaning done).

### The backward: replay

The header says `adjoint 0 instrs, native_adj=0`: no derivative
program was generated for this island, deliberately. Running an
if/else backward would require knowing which instructions belong to
which arm, and this flat list has already forgotten -- the arms are
just address ranges between jumps now.

So the backward is the runtime's fallback, the *replay*: run the
same 11 instructions again, but on stan-math's autodiff scalar
instead of plain doubles, let it build a little heap tape (about
four nodes here), and read `d(m)/d(theta)` off that tape. Because
the comparison reads *values*, the replay takes the same arm the
forward took, so the derivative is the derivative of the branch that
actually ran -- exactly what CmdStan's generated C++ computes for
the same statement.

The replay is slow per instruction, and that is an accepted cost:
branches on parameters are rare in real models (they make the
posterior itself nonsmooth, so modelers avoid them). What matters is
that the model *compiles* and the answer is *right*; a model with
one small branch pays a few tape nodes per gradient and takes the
normal path everywhere else.

### The receipts, by hand

This model is small enough to check on paper. `stanli_check` probes
at `theta` = 0.1, and the data below sets `y` = 0.7:

```
OK -0.12499999999999997 0.99999999999999989
```

At `theta` = 0.1 the condition is true, so `m` = 0.2, and the
dropped-constants log density is -(0.7 - 0.2)²/2 = -0.125. The
gradient is (0.7 - 0.2) · d(m)/d(theta) = 0.5 · 2 = 1. Both come
out right (to the last-bit wobble of computing 0.7 - 0.2 in
floating point), and the 2 in that product is the derivative of the
arm that ran, flowed through the density's backward (Part 1's
machinery), into the island's replay, and out to `theta`.

## Part 3: a recurrence

### The model

```stan
data {
  real y;
}
parameters {
  real theta;
  real<lower=0> sigma;
}
model {
  vector[2] w = [0.8, 0.2]';
  vector[2] s = [theta, theta * 0.5]';
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

A two-element state, updated ten times, each step reading what the
previous step wrote. This is the miniature of an HMM forward pass, a
GARCH volatility update, an AR filter -- the *sequential* model
class. One wrinkle to note in passing: the density is written as
explicit `lp += normal_lpdf(...)`, the keep-all-constants form,
because that is the form the machinery below can express; `~`'s
term-dropping depends on argument types, which the island's uniform
treatment of registers deliberately does not model.

### Why the normal path stalls here

Part 1's unroll-then-reroll trick dies on this loop. Unrolling works
fine -- N is known -- but re-rolling requires proving the iterations
independent, and they are not: iteration t reads the `s` iteration
t-1 wrote. Fusing them into vector ops would change the math. So the
graph keeps all ten iterations as scalar ops:

```
slots=123 ops=113 result=122
SUMMARY ops=113 scalar_out=93 vector_out=20
  ADD                      total=30 scalar=30
  MUL                      total=11 scalar=11
  LGAMMA                   total=10 scalar=10
  NORMAL_LPDF              total=10 scalar=10
  TANHV                    total=10 scalar=10
  DOT                      total=10 scalar=10
  INDEX                    total=10 scalar=10
  CONSTRAIN_LOWER          total=1 scalar=1
```

One hundred thirteen ops, each paying a dispatch, a context, and a
scratch-stash per evaluation, forward and backward. Correct, and
per-op overhead-bound: this is the one model class where the op
graph loses to CmdStan's inlined code. It is also exactly what the
last lowering pass, the *carver*, exists for: find a maximal run of
scalar parameter-only ops and compile it into the same register
machine Part 2 introduced -- this time not because the graph
*cannot* express the region, but because one op with a private
program is cheaper than a hundred ops with a hundred dispatches.

With the carver forced on (it declines this toy; the last layer
explains why), the graph collapses to Part 1's size:

```
slots=124 ops=8 result=123
    0 CONSTRAIN_LOWER v=00 out=s4(len1) in=s1(l1,P),s2(l1)
    1 MUL        v=00 out=s7(len1) in=s0(l1,P),s6(l1)
    2 CONCAT2    v=00 out=s8(len2) in=s0(l1,P),s7(l1)
    3 ISLAND     v=00 out=s122(len2) in=s8(l2),s4(l1)
    4 INDEX      v=00 out=s110(len1) in=s122(l2) idata=[0]
    5 INDEX      v=00 out=s120(len1) in=s122(l2) idata=[1]
    6 ADD        v=00 out=s121(len1) in=s110(l1),s120(l1)
    7 ADD_N      v=00 out=s123(len1) in=s121(l1),s3(l1)
```

Ops 1-2 build the initial state `s` from `theta` as ordinary graph
ops; op 3 is the entire loop; ops 4-7 unpack its two outputs and sum
them with the jacobian.

And `w` is nowhere. `[0.8, 0.2]'` is `DataOnly`, so the MIR
interpreter -- Part 1's slow, handles-everything machine -- computed
it at load and the values became constants. You will meet them again
inside the island's pool.

### Inside the island: one loop iteration

`dump_islands` on op 3, header and first iteration:

```
== island 0 (graph op 3): 123 instrs, 118 regs, 10 calls, adjoint 112 instrs, native_adj=1
  live-ins: slot8->r0[len 2] slot4->r10[len 1]
  live-outs (out_regs): r89 r97

  FORWARD:
    0  CONSTR    r2..r3 <- pool[0..] (=0.8,0.2)
    1  MOVR      r98..r99 <- r0..r1
    2  DOT       r4 <- dot(r0.., r2.., len 2)
    3  TANH      r5 <- r4
    4  MOV       r0 <- r5
    5  MOV       r6 <- r1
    6  CONST     r7 <- pool[2] (=0.9)
    7  MUL       r8 <- r6, r7
    8  MOV       r1 <- r8
    9  CONST     r9 <- pool[3] (=0.7)
   10  DENSITY   r11 <- normal_lpdf(r9, r4, r10)
   11  CONST     r12 <- pool[4] (=2)
   12  ADD       r13 <- r4, r12
   13  CALL      OP_LGAMMA(r13[len 1]) -> r14[len 1], no scratch
   14  ADD       r15 <- r11, r14
   15  CONST     r16 <- pool[5] (=0)
   16  ADD       r17 <- r16, r15
```

Same machine as Part 2, no jumps, and three instructions that earn
their own paragraphs. But first, line the rest up against the Stan
source:

- **Instruction 0** is `w`: the folded constants, loaded from the
  pool. `y` (0.7, absorbed from the data) and the literals 0.9, 2,
  and 0 arrive the same way at instructions 9, 6, 11, and 15.
- **Instruction 2** is `a = dot_product(s, w)`: the state lives in
  registers 0-1, `w` in 2-3.
- **Instructions 3-4** are `s[1] = tanh(a)` -- and notice there is
  no copy of `s`. At the graph level, assigning one element of a
  vector copies the whole vector, every time, because some *other*
  op might still read the old value. Inside the island the compiler
  proved nothing else reads the old state, so all ten per-iteration
  versions of `s` collapsed onto the single register pair 0-1, and
  the assignment is an overwrite. **Registers are reused; that fact
  drives everything about the backward.**
- **Instructions 5-8** are `s[2] = s[2] * 0.9`, the same way.
- **Instructions 14-16** accumulate `lp`.

Now the three special instructions.

**Instruction 10, `DENSITY`.** One instruction that can be any of
the 27 scalar continuous densities; which one rides in the
instruction, and the operands are registers holding `y`, `a`, and
`sigma`. Running it forward calls the ordinary
`stan::math::normal_lpdf` on three plain doubles -- value only,
nothing stashed. (Its backward will recompute the partials instead;
that trade is explained below.)

**Instruction 13, `CALL`.** The machine has no lgamma instruction,
and instead of growing one per function, it has a single instruction
that runs *the graph's own kernel* -- the identical C++ the
113-op graph dispatches for each of its ten `LGAMMA` ops -- over a
range of registers. Any
scalar operation the graph knows, the island can say. A kernel that
stashes partials gets a scratch range carved out of this same
register file; lgamma's backward recomputes from its input, so it
needs none.

**Instruction 1, `MOVR`.** The subtle one, and the key to the
backward pass.

### The checkpoint: saving a value before it is destroyed

The backward of `dot_product` must compute `d(a)/d(w_i) = s_i`: it
needs the *values* of `s` at the moment the dot product ran. But
registers 0-1 are overwritten at instructions 4 and 8, and again in
every following iteration. By the time any backward runs, the values
instruction 2 read are long gone.

The adjoint generator finds exactly this situation. It classifies
every instruction by what its derivative rule needs -- nothing for
additions and copies, its operands' values for multiplies and dots,
its own output's value for `exp` and `tanh` -- and then checks, for
each needed value, whether the register holding it is ever written
again afterward. Almost everything survives: `r4` is written once,
the constants are never touched, `sigma` sits in `r10` untouched.
The one failure in this program is the state pair, ten times over.
So the generator inserts instruction 1: copy registers 0-1 into
fresh registers 98-99 before anything overwrites them. Each
iteration gets its own two-register save. Twenty registers of
insurance, and nothing else in 123 instructions needed any.

This is the trade reverse-mode autodiff always makes: remember
enough of the forward pass to run the chain rule backward. CmdStan
remembers by heap-allocating a tape node per operation, every
evaluation. Here, remembering is two register copies per iteration,
decided once at load, into memory that already exists.

### The generated backward

The header said `adjoint 112 instrs, native_adj=1`: alongside the
forward program, the compiler generated its derivative -- the
forward list read backward, each instruction replaced by its
chain-rule step. This is the difference from Part 2's island: no
jumps means the reversal is a straight walk, so it can be *compiled
once* instead of replayed on tape scalars every evaluation. It runs
over a second array, the *adjoint file*, one cell per register: cell
k holds the derivative of the final answer with respect to the value
currently in register k.

The first steps, trimmed of unused operand fields (they mirror the
*last* forward instructions; `dst`, `a`, `b`, `c` name adjoint
cells, `va`, `vb` name value registers):

```
  ADJOINT (reverse order):
    0  ADD       dst=97 a=94 b=96
    1  CALL      a=9
    2  ADD       dst=95 a=90 b=12
    3  DENSITY   dst=94 a=9 b=90 c=10 | va=9 vb=90 vc=10
    4  MUL       dst=93 a=1 b=7      | va=92 vb=7
    5  MOV       dst=0 a=91
    6  TANH      dst=91 a=90         | va=90
    7  DOT       dst=90 a=0 b=2      | va=116 vb=2
```

Each step does the same three things: take the adjoint waiting in
its output's cell, clear that cell (walking further back, the same
register will mean an older value, which has its own derivative),
and add contributions to its operands' cells. Walk them:

- **Step 0**, the adjoint of `lp += ...`: addition routes its
  adjoint to both operands unchanged. Pure routing, no values.
- **Step 1** is the CALL's backward: the `OP_LGAMMA` kernel's own
  backward function, over the island's registers. The graph and the
  island share one derivative implementation because they share the
  kernel.
- **Step 3** is the density's backward, where most of a real model's
  mathematics lives. It wraps the three argument values in a
  recording scalar and calls the same `stan::math::normal_lpdf`
  template again -- Part 1's machinery exactly, except the partials
  land in a four-double array on the C stack instead of a scratch
  slice, because recomputing them here was cheaper than saving them.
  Three multiply-adds then spread them: `adj(sigma) += t · d/dsigma`,
  and so on. Nothing in stanli knows the derivative of a normal
  density; the Stan library computes it, without building any tape.
- **Step 4**, `s[2] * 0.9`: the multiply rule reads its other
  operand's value, `vb=7` -- the register still holding 0.9, never
  overwritten, so no checkpoint was needed.
- **Step 6**, `tanh`: recomputes its slope from its input's value
  (`va=90`, safe).
- **Step 7** is the payoff. The dot product's rule needs the state
  values, and its value operand is `va=116` -- not register 0. That
  is the checkpoint from the *tenth* iteration's save, being
  consumed. Iteration nine's walk will read `va=114`, and so on down
  to `va=98`. The insurance taken out in the forward is claimed
  here, ten times.

The wiring around the program is short. The island op's scratch
slice *is* its register file, so everything the forward left behind
-- values, checkpoints -- is simply still there when the backward
runs. The backward zeroes the adjoint file, seeds the live-out
registers' cells from the adjoints the graph delivered through the
INDEX ops, runs the 112 steps, and adds the cells of registers 0-1
and 10 into the arena adjoints of `s` and `sigma`. From there the
normal path's backward carries on through `CONCAT2` and `MUL` to
`theta`. Every mechanism in this tutorial is on that one path.

### The estimate says no

A twist to end on: the default build does not carve this toy's loop.
The carver compiled the island, generated its backward, and then
priced it against the ops it would replace
(`STANLI_DEBUG_ISLAND=1` prints the verdict):

```
island? ops=108 graph=658 island=669
```

The graph side counts the data its ops move plus a fixed per-op
dispatch tax. The island side counts its registers (written by the
forward, read by the backward), both instruction lists, and -- since
a CALL runs a graph kernel with graph overhead -- the same per-op
tax for each of the ten CALLs; absorbing a CALL buys continuity of
the region, never speed. 658 against 669: at ten iterations of this
shape the island is a wash, so the ops stay, and correctness is
identical either way. Every island listing above was produced with
`STANLI_ISLAND_ALWAYS=1`, the switch whose purpose is asking "what
would you have built, and why did you decline?" Scale the same shape
up -- more steps, a wider state, HMM emissions -- and the ratio
flips: the sequential models in [benchmarks.md](benchmarks.md) carve
regions of tens of thousands of ops down to a dozen instructions,
run 1.4-4.7x faster than they do with islands off, and cross parity
with CmdStan's compiled code.

### The receipts

Three configurations, same model, same point:

```
default          OK -10.99081500923419 6.3614616764009035 -4.8799704452531198
islands forced   OK -10.99081500923419 6.3614616764009035 -4.8799704452531198
islands off      OK -10.99081500923419 6.3614616764009035 -4.8799704452531198
```

Log density, d/d`theta`, d/d`sigma` -- identical to the last digit
whether the loop ran as 113 graph ops or as one island under the
generated backward. That invariant is what every layer above is
built to preserve, and the corpus gate holds all 119 models to it
against recorded CmdStan values on every push.

## Reproduce it

From a built checkout (see [hacking.md](hacking.md)), save the three
models as `ex1.stan`, `ex2.stan`, `ex3.stan`, and two data files:

```json
{"N": 5, "x": [-2, -1, 0, 1, 2], "y": [-1.9, -1.1, 0.2, 0.9, 2.1]}
```

as `ex1.json`, and `{"y": 0.7}` as `ex23.json`. Then, for each
model, produce the compiler tree once:

```
./deps/stanc3/stanc --debug-transformed-mir ex1.stan > ex1.sexp
```

Part 1:

```
build-rel/dump_ops ex1.sexp ex1.json          # the op graph
build-rel/stanli_check ex1.stan ex1.json      # the receipts
```

Part 2:

```
build-rel/dump_ops     ex2.sexp ex23.json     # ISLAND + the normal path
build-rel/dump_islands ex2.sexp ex23.json     # the jump program
build-rel/stanli_check ex2.stan ex23.json     # check it by hand
```

Part 3:

```
STANLI_NO_ISLAND=1     build-rel/dump_ops ex3.sexp ex23.json -1   # 113 ops
STANLI_ISLAND_ALWAYS=1 build-rel/dump_ops ex3.sexp ex23.json      # 8 ops
STANLI_ISLAND_ALWAYS=1 build-rel/dump_islands ex3.sexp ex23.json  # fwd + adjoint
STANLI_DEBUG_ISLAND=1  build-rel/stanli_check ex3.stan ex23.json  # the estimate
build-rel/stanli_check ex3.stan ex23.json                         # receipts x3
STANLI_ISLAND_ALWAYS=1 build-rel/stanli_check ex3.stan ex23.json
STANLI_NO_ISLAND=1     build-rel/stanli_check ex3.stan ex23.json
```

Slot and register numbers may shift as the compiler evolves; the
shapes -- the eight-op regression, the jumps, the collapsed state
pair, the checkpoints, the DENSITY and CALL instructions, the
mirrored backward -- are the load-time artifacts this tutorial is
about.
