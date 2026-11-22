# Tutorial: three small models through every layer

This tutorial follows three small models through each layer of stanli,
from Stan source to gradient. Together they cover the whole runtime. The
models are in order of how often their shape occurs in practice:

1. **A linear regression.** This is the normal path. Nearly every real
   model takes it, and it is the reason that stanli is usually faster
   than a compiled model.
2. **A branch on a parameter.** A fixed list of operations cannot
   express a branch like this. You will see how stanli handles it.
3. **A recurrence.** No pass can vectorize this loop. stanli compiles
   it into one region with a generated derivative, which makes it fast
   anyway.

Every listing below is real tool output. The last section shows how to
reproduce all of it.

If you have not read [how-it-works.md](how-it-works.md), here is the one
paragraph that you need from it. stanli does not generate machine code.
It turns a model into a list of operations over flat arrays. It runs
the list forward to get the value and backward to get the gradient.
Because the list never changes, it also serves as the autodiff tape.
This tutorial works through that paragraph in detail, three times.

These terms appear throughout:

| word | meaning |
| --- | --- |
| slot | one value in the graph: a scalar, vector, or matrix, living at a fixed place in one big array (the arena) |
| op | one step of the graph: reads some slots, writes a slot |
| kernel | the C++ that implements an op: a `forward` function and a `backward` function |
| adjoint | the running derivative of the final answer with respect to some value; the backward pass reads and updates these |
| scratch | per-op working memory, sized at load time, where a forward saves what its backward will need |
| register | one *number* in an island's private array; a vector is a run of consecutive registers |
| pool | an island's table of constants, filled in at load time |

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

This is a linear regression with priors. It has no branches and no
loops. Most models that people run have this shape, and what stanli
does here carries over directly to GLMs and hierarchical models.

### Layer 1: the compiler's tree

stanli does not parse Stan itself. It links in the official compiler,
stanc3, and asks it for its typed intermediate representation, the MIR.
`stanc --debug-transformed-mir` prints the MIR. Here is the sampling
statement, trimmed:

```
(FunApp (StanLib normal_lpdf (FnLpdf true) AoS)
  ((Var y)   ... (adlevel DataOnly) ...
   (FunApp (StanLib Plus ...))  ... (adlevel AutoDiffable) ...))
```

Two annotations on this tree control everything that follows.

**`adlevel`** is the result of the compiler's taint analysis.
`AutoDiffable` means that a parameter reaches this expression.
`DataOnly` means that the expression is fixed once the data is read.
Lowering trusts these labels completely. A `DataOnly` subtree never
becomes ops. stanli evaluates it once, at load, and its result becomes
a constant. The gradient has to flow through an `AutoDiffable` subtree,
so that subtree must become something differentiable.

**`(FnLpdf true)`** says that the model wrote this density with `~`, so
its constant terms are dropped. This is the `lup` ("log unnormalized
probability") in the internal name that CmdStan uses. Which terms get
dropped depends on which arguments are parameters. The `adlevel` of
each argument gives exactly that information, and Layer 2 uses it.

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

Eight ops cover the whole model. Read the columns first. Each op writes
one output slot (`out=s10(len1)` means slot 10, one element) and reads
input slots. `P` marks a slot that is part of the unconstrained
parameter vector, and `l5` is a length. A slot is a fixed offset into
one flat value array, the *arena*. stanli assigns slots at load time
and never reassigns them. Reverse mode uses a second, compact arena. It
holds the parameters first, then only the slots that surviving ops
write (plus a result that would otherwise be constant). Data slots, and
slots that graph rewrites left behind, need no adjoint cell, so stanli
does not clear them on every gradient.

Now read the ops as the model:

- **Op 0** turns the unconstrained `sigma` (slot 2) into the positive
  `sigma` that the model sees (slot 5). Its second output, slot 4, is
  the log-jacobian of that transform, which joins the target at the
  end. Every constrained parameter enters a model this way.
- **Ops 1-2** compute `alpha + beta * x`: one multiply of a scalar by a
  length-5 vector, then one broadcast add. The data vector `x` is slot
  7. No op produces it, because stanli fills data slots once at load.
- **Op 3** is the sampling statement. It is one op for any N. The
  observed `y` (slot 6), the predicted mean vector (slot 9) and `sigma`
  go in. One number comes out, the summed log density.
- **Ops 4-6** are the three priors. They use the same density with
  scalar arguments.
- **Op 7** sums the four density terms and the jacobian into the final
  log density, slot 15.

The op count matters most. This graph has 8 ops at N = 5 and still 8
ops at N = 5,000,000. Only the slot lengths grow. CmdStan's generated
C++ handles the same statement by building one heap-allocated autodiff
tape node per operation *on every evaluation*. Its cost per gradient
therefore grows with N, in both time and allocation. For stanli, each
evaluation costs eight kernel dispatches over preallocated arrays. This
is why models with vectorized statements (most of the corpus) run
1.5-6x faster than CmdStan's compiled code
([benchmarks.md](benchmarks.md)).

The `v=` column is the *variant* byte. It is worth decoding once,
because this is where the `adlevel` labels end up. For a density op,
bits 0-5 mark which arguments are autodiffable, and bit 7 marks the
dropped-constants form. Op 3 has `v=86` (hex). That means propto, with
arguments 1 and 2 (the mean and `sigma`) active and argument 0 (the
data `y`) inactive. Ops 4-6 have `v=81`, which means propto with only
argument 0 active. In a prior, the "observation" is the parameter. The
kernel uses this byte to instantiate exactly the data/parameter
combination that CmdStan's generated code would have used. That is why
the dropped constants, and therefore the log density, match CmdStan
bitwise.

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

The result is the same eight ops. Because N is known once the data is
loaded, lowering unrolls the loop into five scalar densities and five
scalar multiplies. The optimization passes then roll it back up. They
find five ops that differ only by index, prove that the iterations are
independent, and fuse them into the vector ops above. (The higher slot
count is left over from numbering the unrolled intermediates. Nothing
computes them anymore.) This unroll and reroll is why the loop form and
the vectorized form cost the same. It is also why hierarchical models
that use the indexing idiom `y[n] ~ normal(mu[county[n]], sigma)` still
lower to a handful of vector ops.

The graph above is what remains after all passes, and that graph is
*final*. It contains no decisions. Every remaining question (which op,
which slots, which lengths, which variant) was answered at load time.

### Layer 3: forward, or where the math lives

At load, the executor walks the op list once and binds each op to a
*kernel context*. The context holds raw pointers into the arenas for
each input and output, plus a slice of a third preallocated array,
*scratch*. Evaluating the model is then a for-loop that calls each
op's `forward` through a function-pointer table, from 0 through 7.

Most forward functions do what you would write yourself: `ADD_N` sums
and `MUL` multiplies. The density kernel is more interesting, because a
density forward has a second job. It must save what the backward will
need. `NORMAL_LPDF`'s forward computes the summed log density. It also
writes into its scratch slice the partial derivative of the result with
respect to every element of every active argument. For op 3, that is
five partials for the mean vector and one accumulated partial for
`sigma`.

These partials come from the Stan library, and stanli has no code of
its own for them. The kernel binds each active argument as a small
recording scalar and calls the same `stan::math::normal_lpdf` template
that CmdStan calls. stan-math's operands-and-partials code computes the
value and the per-argument partials in plain doubles. The recorder only
points them at the scratch slice. More than seventy densities get their
derivatives this way. Each one uses the Stan library's own math, and
stanli duplicates none of it.

### Layer 4: backward, or the list read in reverse

There is no separate tape, because the op list is the tape. The list
was fixed at load, so nothing needs to be recorded on each evaluation.
To compute the gradient, stanli zeroes the adjoint arena, sets the
result slot's adjoint to 1, and calls each op's `backward` in reverse
order, from 7 down to 0.

Each backward reads the adjoint of its output slot and adds
contributions to the adjoints of its inputs:

- **Op 7** (`ADD_N`): addition passes its adjoint to every operand
  unchanged. The four density slots and the jacobian slot now hold
  adjoint 1.
- **Ops 6-3** (the densities) multiply each partial saved in scratch by
  the output adjoint and add the product into the argument's adjoint
  slot. Op 3's backward adds five numbers into the adjoints of the mean
  vector and one into the adjoint of `sigma`. The data argument gets
  nothing, as the variant byte specified at load.
- **Ops 2-1** apply the rules for broadcast add and scalar-times-vector.
  They sum the vector adjoints back onto `alpha` and `beta`.
- **Op 0** multiplies the adjoint of the constrained `sigma` by the
  slope of the transform, adds the jacobian term's adjoint, and writes
  the result to the adjoint of unconstrained slot 2.

After op 0, the adjoints of slots 0-2 are the gradient. They sit in
declaration order in a flat array that the sampler reads directly.
Neither direction allocates memory. The whole evaluation touches three
preallocated arrays through pointers that were bound once.

### The receipts

`stanli_check` loads the model, evaluates it once at a fixed point, and
prints the log density and gradient. (It first prints a `WA` line that
checks the write_array path, which this tutorial does not cover.)

```
OK -6.1763422914570461 -0.46642082744804991 12.214027581601695 7.5052230767581278
```

The line shows lp, then d/d`alpha`, d/d`beta` and d/d`sigma`. CmdStan's
compiled model produces the same numbers at the same point. The corpus
check replays 119 posteriordb models against recorded CmdStan values on
every push. Dozens of them match bitwise, and the worst is within about
3e-12 relative.

One more part of the runtime belongs here. Everything in a Stan program
that is *not* the log density (transformed data, generated quantities,
`print` statements) runs on a plain tree-walking interpreter over the
MIR. The interpreter keeps names and Stan semantics and does no
lowering. It also computes any `DataOnly` expression that the graph
needs as a constant (Part 3 shows it folding a vector). It is a few
hundred lines long, it handles everything, and it is slow. That is
fine, because none of its work is inside the sampler's inner loop. When
the fast path cannot express something, the interpreter can, so the
fast path can stay small.

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

The model has one decision and one density. This example is about the
decision. The density gives the branch something downstream that uses
its result.

### Why the graph cannot say this

In the MIR, the condition has the annotation that causes the problem:

```
(IfElse ((FunApp (StanLib Greater__ ...)
           (Var theta)  ... (adlevel AutoDiffable) ...
           (Lit Int 0)  ... (adlevel DataOnly) ...)) ...)
```

`adlevel AutoDiffable` on an `if` condition means that stanli cannot
resolve the branch at load time. If the condition depended only on
data, lowering would evaluate it once and lower only the arm that it
selects. A condition on data costs nothing. This condition changes with
`theta` at every leapfrog step. Part 1 showed the defining property of
the graph: it contains no decisions. A fixed list of ops cannot contain
an `if`.

stanli keeps the list fixed and puts the decision inside one op:

```
slots=6 ops=3 result=5
    0 ISLAND     v=00 out=s1(len1) in=s0(l1,P)
    1 INDEX      v=00 out=s2(len1) in=s1(l1) idata=[0]
    2 NORMAL_LPDF v=82 out=s5(len1) in=s3(l1),s2(l1),s4(l1)
```

Op 0 is an *island*, an op that contains its own small program. It is
the entire if/else: `theta` goes in and `m` comes out. Op 1 unpacks the
island's output vector into the scalar slot that the density reads. Op
2 is Part 1's density op again (`v=82`: propto, only the mean argument
active). After the island, everything follows the normal path. The
density does not know that its mean came out of a branch.

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

The program uses numbered registers, a constant pool, and an
instruction list that runs from top to bottom. Unlike the graph, it has
*jump instructions*. "Live-in `slot0 -> r2`" means: before running,
copy graph slot 0 (`theta`) into register 2. "Live-out `r1`" means:
after running, the op's output is whatever register 1 holds.

Read it as the if statement. Register 1 is `m`. It starts as NaN
because Stan defines an unassigned local variable to be NaN, and
instruction 1 applies that rule. Instruction 3 compares `theta` with
zero and writes the answer *as a number*, 0 or 1, into register 4.
Instruction 4 turns that number back into control flow: it jumps to
the else-arm when the number is zero. Both arms end by writing register
1, so the result is in the same place whichever arm ran. Instruction 8
jumps past the else-arm. ("goto 11" is one past the end, which means
done.)

### The backward: replay

The header says `adjoint 0 instrs, native_adj=0`. stanli generated no
derivative program for this island, and that is intended. To run an
if/else backward, the generator would need to know which instructions
belong to which arm. This flat list no longer has that information,
because the arms are now only address ranges between jumps.

So the backward uses the runtime's fallback, called *replay*. It runs
the same 11 instructions again on stan-math's autodiff scalar instead
of plain doubles. That builds a small heap tape (about four nodes
here), and stanli reads `d(m)/d(theta)` from that tape. The comparison
reads *values*, so the replay takes the same arm that the forward took.
The result is the derivative of the branch that ran, which is what
CmdStan's generated C++ computes for the same statement.

Replay is slow per instruction. stanli accepts that cost because
branches on parameters are rare in real models. They make the
posterior itself nonsmooth, so modelers avoid them. What matters is
that the model *compiles* and the answer is *right*. A model with one
small branch spends a few tape nodes per gradient and takes the normal
path everywhere else.

### The receipts, by hand

This model is small enough to check on paper. `stanli_check` evaluates
at `theta` = 0.1, and the data below sets `y` = 0.7:

```
OK -0.12499999999999997 0.99999999999999989
```

At `theta` = 0.1 the condition is true, so `m` = 0.2. The
dropped-constants log density is -(0.7 - 0.2)²/2 = -0.125. The gradient
is (0.7 - 0.2) · d(m)/d(theta) = 0.5 · 2 = 1. Both values are right,
apart from the last-bit error of computing 0.7 - 0.2 in floating point.
The 2 in that product is the derivative of the arm that ran. It flowed
through the density's backward (from Part 1), into the island's replay,
and out to `theta`.

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

The model keeps a two-element state and updates it ten times. Each step
reads what the previous step wrote. This is a miniature HMM forward
pass, GARCH volatility update or AR filter, all of which are
*sequential* models. Note one detail. The density is written as an
explicit `lp += normal_lpdf(...)`, which keeps all constants, because
the island code below can only express this form. Dropping terms with
`~` depends on argument types. The island treats all registers the same
way, so it does not model those types.

### Why the normal path stalls here

Part 1's unroll and reroll fails on this loop. Unrolling works, because
N is known. Rerolling requires a proof that the iterations are
independent, and they are not: iteration t reads the `s` that iteration
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

That is 113 ops. Each one costs a dispatch, a context and a scratch
write per evaluation, both forward and backward. The result is correct,
but per-op overhead limits its speed. This is the one model class where
the op graph loses to CmdStan's inlined code. The last lowering pass,
the *carver*, exists for this case. It finds a maximal run of scalar,
parameter-only ops and compiles it into the same register machine that
Part 2 introduced. Here the graph *can* express the region. The carver
compiles it anyway, because one op with a private program is cheaper
than a hundred ops with a hundred dispatches.

With the carver forced on, the graph shrinks to Part 1's size. (By
default the carver declines this toy model. The last layer explains
why.)

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

Ops 1-2 build the initial state `s` from `theta` as ordinary graph ops.
Op 3 is the entire loop. Ops 4-7 unpack its two outputs and add them to
the jacobian.

`w` does not appear anywhere. `[0.8, 0.2]'` is `DataOnly`, so the MIR
interpreter from Part 1 (the slow one that handles everything) computed
it at load, and its values became constants. You will see them again
inside the island's pool.

### Inside the island: one loop iteration

Here is `dump_islands` on op 3, showing the header and the first
iteration:

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

This is the same machine as in Part 2, without jumps. Three of its
instructions need their own paragraphs below. First, match the rest
against the Stan source:

- **Instruction 0** is `w`. It loads the folded constants from the
  pool. `y` (0.7, taken from the data) and the literals 0.9, 2 and 0
  arrive the same way, at instructions 9, 6, 11 and 15.
- **Instruction 2** is `a = dot_product(s, w)`. The state lives in
  registers 0-1, and `w` lives in registers 2-3.
- **Instructions 3-4** are `s[1] = tanh(a)`. Note that there is no copy
  of `s`. At the graph level, assigning one element of a vector copies
  the whole vector every time, because some *other* op might still read
  the old value. Inside the island, the compiler proved that nothing
  else reads the old state. So all ten per-iteration versions of `s`
  share the single register pair 0-1, and the assignment overwrites it.
  Registers are reused, and that fact shapes everything about the
  backward.
- **Instructions 5-8** compute `s[2] = s[2] * 0.9` in the same way.
- **Instructions 14-16** add to `lp`.

Now the three special instructions.

**Instruction 10, `DENSITY`.** This one instruction can be any of the
27 scalar continuous densities. The instruction records which one, and
its operands are the registers that hold `y`, `a` and `sigma`. The
forward calls the ordinary `stan::math::normal_lpdf` on three plain
doubles. It computes only the value and saves nothing. (Its backward
recomputes the partials instead. The section on the backward explains
this choice.)

**Instruction 13, `CALL`.** The machine has no lgamma instruction.
Instead of adding one instruction per function, it has a single
instruction that runs *the graph's own kernel* over a range of
registers. This is the same C++ that the 113-op graph runs for each of
its ten `LGAMMA` ops. So the island can express any scalar operation
that the graph knows. A kernel that saves partials gets a scratch range
taken from the same register file. lgamma's backward recomputes from
its input, so it needs no scratch.

**Instruction 1, `MOVR`.** This one is subtle, and it is the key to the
backward pass.

### The checkpoint: saving a value before it is destroyed

The backward of `dot_product` must compute `d(a)/d(w_i) = s_i`. It
needs the *values* of `s` at the moment the dot product ran. But
instructions 4 and 8 overwrite registers 0-1, and every later iteration
overwrites them again. By the time any backward runs, the values that
instruction 2 read are gone.

The adjoint generator detects this case. It sorts every instruction by
what its derivative rule needs. Additions and copies need nothing.
Multiplies and dots need their operands' values. `exp` and `tanh` need
their own output value. For each needed value, the generator then
checks whether any later instruction writes the register that holds it.
Almost every value survives: `r4` is written once, the constants are
never touched, and `sigma` stays in `r10`. The one exception in this
program is the state pair, ten times over. So the generator inserts
instruction 1, which copies registers 0-1 into fresh registers 98-99
before anything overwrites them. Each iteration gets its own
two-register copy. That is twenty extra registers, and no other value
in the 123 instructions needed one.

Reverse-mode autodiff always makes this trade. It must remember enough
of the forward pass to run the chain rule backward. CmdStan remembers
by allocating a tape node on the heap for every operation, on every
evaluation. Here, remembering costs two register copies per iteration.
stanli decides this once at load, and the copies go into memory that
already exists.

### The generated backward

The header said `adjoint 112 instrs, native_adj=1`. Along with the
forward program, the compiler generated its derivative. The derivative
program is the forward list read backward, with each instruction
replaced by its chain-rule step. This is how it differs from Part 2's
island. Without jumps, reversing the program is a straight walk, so
stanli can *compile it once* instead of replaying it on tape scalars on
every evaluation. It runs over a second array, the *adjoint file*, with
one cell per register. Cell k holds the derivative of the final answer
with respect to the value currently in register k.

Here are the first steps, with unused operand fields removed. They
mirror the *last* forward instructions. `dst`, `a`, `b` and `c` name
adjoint cells, and `va` and `vb` name value registers:

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

Each step does the same three things. It takes the adjoint waiting in
its output's cell. It clears that cell, because further back the same
register holds an older value with its own derivative. Then it adds
contributions to its operands' cells. Here is what each step does:

- **Step 0** is the adjoint of `lp += ...`. Addition passes its adjoint
  to both operands unchanged. It reads no values.
- **Step 1** is the CALL's backward: the `OP_LGAMMA` kernel's own
  backward function, run over the island's registers. The graph and the
  island share the kernel, so they share one derivative implementation.
- **Step 3** is the density's backward. Most of a real model's math
  happens in steps like this one. It wraps the three argument values in
  a recording scalar and calls the same `stan::math::normal_lpdf`
  template again. This is the same method as in Part 1, except that the
  partials go into a four-double array on the C stack instead of a
  scratch slice. Recomputing them here was cheaper than saving them.
  Three multiply-adds then apply them: `adj(sigma) += t · d/dsigma`,
  and so on. No code in stanli knows the derivative of a normal
  density. The Stan library computes it, without building any tape.
- **Step 4** is `s[2] * 0.9`. The multiply rule reads its other
  operand's value, `vb=7`. That register still holds 0.9 and is never
  overwritten, so it needed no checkpoint.
- **Step 6** is `tanh`. It recomputes its slope from its input's value
  (`va=90`, which is safe).
- **Step 7** uses the checkpoint. The dot product's rule needs the
  state values, and its value operand is `va=116` instead of register
  0. Register 116 holds the copy made in the *tenth* iteration.
  Iteration nine's step will read `va=114`, and so on down to `va=98`.
  This is where the backward uses the ten copies made in the forward.

The code around the program is short. The island op's scratch slice
*is* its register file, so everything the forward left there (values
and checkpoints) is still there when the backward runs. The backward
zeroes the adjoint file and seeds the cells of the live-out registers
from the adjoints that the graph passed in through the INDEX ops. Then
it runs the 112 steps and adds the cells of registers 0-1 and 10 into
the arena adjoints of `s` and `sigma`. From there, the normal backward
continues through `CONCAT2` and `MUL` to `theta`. Every mechanism in
this tutorial is on that one path.

### The estimate says no

By default, stanli does not carve this toy loop. The carver compiled
the island and generated its backward. Then it compared the island's
estimated cost with the cost of the ops that it would replace.
(`STANLI_DEBUG_ISLAND=1` prints the result.)

```
island? ops=108 graph=658 island=669
```

The graph estimate counts the data that its ops move, plus a fixed cost
for dispatching each op. The island estimate counts its registers
(written by the forward, read by the backward) and both instruction
lists. A CALL runs a graph kernel with graph overhead, so the island
estimate also adds the same per-op cost for each of the ten CALLs.
Including a CALL in an island keeps the region in one piece but never
makes it faster. The estimates are 658 for the graph and 669 for the
island. At ten iterations of this shape the two are about equal, so the
ops stay. The results are identical either way.

Every island listing above was made with `STANLI_ISLAND_ALWAYS=1`. That
switch exists to show what the carver would have built and why it
declined. If you scale the same shape up, with more steps, a wider
state or HMM emissions, the comparison reverses. The tape-island
section of [`OPTIMIZATIONS.md`](../runtime/src/OPTIMIZATIONS.md) covers
sequential models where the carver turns regions of tens of thousands
of ops into a dozen instructions. Those models run 1.4-4.7x faster than
they do with islands off, and reach parity with CmdStan's compiled
code.

### The receipts

Three configurations, same model, same point:

```
default          OK -10.99081500923419 6.3614616764009035 -4.8799704452531198
islands forced   OK -10.99081500923419 6.3614616764009035 -4.8799704452531198
islands off      OK -10.99081500923419 6.3614616764009035 -4.8799704452531198
```

The columns are the log density, d/d`theta` and d/d`sigma`. They match
to the last digit whether the loop ran as 113 graph ops or as one
island with the generated backward. Every layer above is built to keep
this property. On every push, the corpus check holds all 119 models to
it against recorded CmdStan values.

## Reproduce it

From a built checkout (see [hacking.md](hacking.md)), save the three
models as `ex1.stan`, `ex2.stan` and `ex3.stan`. Save this data as
`ex1.json`:

```json
{"N": 5, "x": [-2, -1, 0, 1, 2], "y": [-1.9, -1.1, 0.2, 0.9, 2.1]}
```

Save `{"y": 0.7}` as `ex23.json`. Then produce the compiler tree once
for each model, unoptimized as in this walkthrough (the same command for
`ex2` and `ex3`):

```
build-rel/stanli_check --emit-mir ex1.stan --passes none > ex1.mir
```

Part 1:

```
build-rel/dump_ops ex1.mir ex1.json          # the op graph
build-rel/stanli_check ex1.stan ex1.json      # the receipts
```

Part 2:

```
build-rel/dump_ops     ex2.mir ex23.json     # ISLAND + the normal path
build-rel/dump_islands ex2.mir ex23.json     # the jump program
build-rel/stanli_check ex2.stan ex23.json     # check it by hand
```

Part 3:

```
STANLI_NO_ISLAND=1     build-rel/dump_ops ex3.mir ex23.json -1   # 113 ops
STANLI_ISLAND_ALWAYS=1 build-rel/dump_ops ex3.mir ex23.json      # 8 ops
STANLI_ISLAND_ALWAYS=1 build-rel/dump_islands ex3.mir ex23.json  # fwd + adjoint
STANLI_DEBUG_ISLAND=1  build-rel/stanli_check ex3.stan ex23.json  # the estimate
build-rel/stanli_check ex3.stan ex23.json                         # receipts x3
STANLI_ISLAND_ALWAYS=1 build-rel/stanli_check ex3.stan ex23.json
STANLI_NO_ISLAND=1     build-rel/stanli_check ex3.stan ex23.json
```

Slot and register numbers may change as the compiler changes. The
shapes stay the same: the eight-op regression, the jumps, the collapsed
state pair, the checkpoints, the DENSITY and CALL instructions, and the
mirrored backward. This tutorial is about those load-time results.
