# One small model, every layer

This follows a single model from Stan source to a finished gradient,
showing the real output of every stage: the compiler's tree, the op
graph, both kinds of island program, the checkpoint the value analysis
inserts, the generated backward, and the cost estimate saying no. Every
listing below is genuine tool output, reproducible with:

```
stanc --debug-transformed-mir toy.stan > toy.sexp
build-rel/dump_ops     toy.sexp toy.json          # the graph
build-rel/dump_islands toy.sexp toy.json          # the layer below it
```

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
  // A branch on a parameter: cannot become graph ops at all.
  real m;
  if (theta > 0) {
    m = 2 * theta;
  } else {
    m = -theta;
  }
  // Data-only: folded to a constant at load by the MIR interpreter.
  vector[2] w = [0.8, 0.2]';
  // A recurrence: each step reads the state the previous step wrote.
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

Five different mechanisms will fire: the branch becomes a *necessity
island* with real jumps; `w` is evaluated once by the MIR interpreter
and becomes a pool constant; the loop becomes a *carved island* whose
state lives in two reused registers; `dot_product` forces a value
checkpoint; `normal_lpdf` becomes a `DENSITY` instruction and `lgamma`
-- which the register machine has no instruction for -- becomes a
`CALL` to the graph's own kernel.

## Layer 1: the compiler's tree (MIR)

`stanc --debug-transformed-mir` emits a typed syntax tree. Two
excerpts, with the parts that matter marked:

```
(IfElse ((FunApp (StanLib Greater__ ...)
           (Var theta) ...(adlevel AutoDiffable)...
           (Lit Int 0)  ...(adlevel DataOnly)...)) ...)

(FunApp (StanLib normal_lpdf (FnLpdf false) AoS)
  ((Var y) ...))
```

`adlevel AutoDiffable` on `theta` is the compiler saying "a parameter
reaches this expression" -- which is what tells lowering the `if`
cannot be decided at load time. `FnLpdf false` says this density call
keeps every constant term (the `lp += normal_lpdf(...)` form), which
is the only form the register machine reproduces.

## Layer 2: the op graph

Lowering walks the tree and emits ops. `dump_ops` on the result:

```
slots=125 ops=12 result=124
  0 CONSTRAIN_LOWER out=s4      in=s1(P),s2
  1 ISLAND          out=s5      in=s0(P)
  2 INDEX           out=s6      in=s5
  3 MUL             out=s9      in=s6,s8
  4 CONCAT2         out=s10     in=s6,s9
  5 ISLAND          out=s123    in=s10,s4
  6 INDEX           out=s114    in=s123
  7 INDEX           out=s121    in=s123
  8 ADD             ...                       (lp terms into the target)
```

Reading it: op 0 constrains `sigma` (slot 1 is the unconstrained
parameter, the second output is its jacobian term). Op 1 is the
*branch*, already an island, with `theta` (slot 0) as its one input and
`m` as its output. Ops 2-4 build `s = [m, m * 0.5]'` as ordinary graph
ops. Op 5 is the *whole loop*: with islands disabled the same model is
115 ops -- 10 iterations of DOT, TANHV, MUL, NORMAL_LPDF, LGAMMA, ADDs
-- and the carver collapsed 108 of them into this one op. Ops 6-7
extract the island's two live-outs back into the slots the rest of the
graph reads.

Where is `w`? Nowhere. `[0.8, 0.2]'` depends on no parameter, so the
MIR interpreter evaluated it during lowering and the values went into a
constant pool. They will reappear below, inside the island.

## Layer 3: the branch island

`dump_islands` prints op 1's program:

```
11 instrs, 8 regs, adjoint 0 instrs, native_adj=0
live-ins: slot0 -> r2      live-outs: r1

  0  CONST  r1 <- pool (=nan)        m starts as NaN, Stan's rule for
  3  GT     r4 <- r2, r3             an unassigned local
  4  JZ     if r4 == 0 goto 9        the predicate is a VALUE (0/1)
  5  CONST  r5 <- pool (=2)
  6  MUL    r6 <- r5, r2             then-arm: m = 2 * theta
  7  MOV    r1 <- r6
  8  JMP    goto 11
  9  NEG    r7 <- r2                 else-arm: m = -theta
 10  MOV    r1 <- r7
```

The comparison materializes its answer into a register; the jump reads
it. Both arms write `r1`, so whichever ran, the live-out is there.

`adjoint 0 instrs, native_adj=0`: the generated backward refused this
program, because running an if/else backward needs to know which
instructions belong to which arm, and the flat list has already thrown
that structure away. So this island's backward is the old replay: run
the same 11 instructions again with stan-math's autodiff scalar, let
it build a four-node tape, pull out d(m)/d(theta). The comparison
reads *values*, so the replay takes the same arm the forward took --
differentiating the branch that actually ran, exactly as CmdStan's
generated C++ does. Branches on parameters are rare; slow is fine;
what matters is that this compiles at all.

## Layer 4: the recurrence island

Op 5's program, first iteration (of ten identical blocks):

```
123 instrs, 118 regs, 10 calls, adjoint 112 instrs, native_adj=1
live-ins: slot10 -> r0[len 2]  (s)     slot4 -> r10  (sigma)
live-outs: r89 r97

  0  CONSTR  r2..r3  <- pool (=0.8,0.2)     w, folded at load
  1  MOVR    r98..r99 <- r0..r1             CHECKPOINT (see below)
  2  DOT     r4 <- dot(r0.., r2.., len 2)   a = dot_product(s, w)
  3  TANH    r5 <- r4
  4  MOV     r0 <- r5                       s[1] = tanh(a)  -- in place
  5  MOV     r6 <- r1
  6  CONST   r7 <- pool (=0.9)
  7  MUL     r8 <- r6, r7
  8  MOV     r1 <- r8                       s[2] = s[2] * 0.9  -- in place
  9  CONST   r9 <- pool (=0.7)              y, absorbed from data
 10  DENSITY r11 <- normal_lpdf(r9, r4, r10)
 11  CONST   r12 <- pool (=2)
 12  ADD     r13 <- r4, r12
 13  CALL    OP_LGAMMA(r13) -> r14, scratch len 0
 14  ADD     r15 <- r11, r14                lp accumulation
```

Everything from the earlier layers lands here. The state `s` lives in
`r0..r1` and is overwritten *in place* each iteration -- at the graph
level each `s[1] = ...` made a fresh copy of the vector, and the
carver's aliasing collapsed all ten copies onto one register pair.
`w` and `y` are pool constants. `normal_lpdf` is one `DENSITY`
instruction: forward calls the plain-double stan-math template, value
only, no scratch. `lgamma` has no instruction of its own, so it is a
`CALL` to the graph's `OP_LGAMMA` kernel -- same code the graph would
have run (a unary needs no scratch; a cdf here would show a scratch
range carved out of the register file).

**The checkpoint.** Instruction 1 is the value layer at work. The
adjoint of `DOT` needs the *values* of `s` from this iteration
(`d(a)/d(w_i) = s_i`), but `r0..r1` are overwritten at instructions 4
and 8 and again every iteration after. The generator saw that -- "a
register the derivative reads is written again later" -- and had the
forward save the pair into fresh registers `r98..r99` first. Ten
iterations, ten two-register saves; nothing else in the program needed
one, because every other value the backward reads (`r4`, `r7`, `r10`,
...) is written once and survives.

## Layer 5: the generated backward

The adjoint program, first steps (reverse order of the forward; `dst`,
`a`, `b`, `c` are adjoint cells, `va`.. are value registers):

```
  0  ADD      dst=97 a=94 b=96            lp = lpdf + lgamma: route
  1  CALL     a=9                          lgamma: kernel's own backward
  2  ADD      dst=95 a=90 b=12            a + 2: route
  3  DENSITY  dst=94 a=9 b=90 c=10 ...    recorder partials, 3 fmas
  4  MUL      dst=93 a=1 b=7  va=92 vb=7  s[2]*0.9: adj += 0.9 * t
  5  MOV      dst=0 a=91                  in-place write: route + clear
  6  TANH     dst=91 a=90     va=90       recomputes cosh(a)
  7  DOT      dst=90 a=0 b=2  va=116 ...  reads the CHECKPOINT
```

Each line consumes its output's adjoint, clears the cell, and
accumulates into its operands, exactly the tape's semantics without a
tape. Step 3 is the 27-density path: bind three doubles as the
recording scalar, call the same `normal_lpdf` template, stan-math
deposits three partials into a stack array, three multiply-adds. Step
1 is the CALL path: the `OP_LGAMMA` kernel's own backward. And step 7
is the checkpoint being *consumed*: its value operand is `va=116` --
the tenth iteration's saved copy -- because by the time the backward
reaches any iteration's `DOT`, `r0..r1` hold a later iteration's
state.

The executor wires this together: the island op's scratch slice *is*
this register file, so the forward leaves values and checkpoints in
place; the backward zeroes an adjoint file, seeds the two live-out
cells from the extraction ops' adjoints, runs the 112 instructions,
and adds the cells for `r0..r1` and `r10` into the arena adjoints of
`s` and `sigma`.

## Layer 6: the estimate says no

On this toy, the default build does not actually carve the loop:

```
island? ops=108 graph=658 island=669
```

The graph side is what the 108 ops move plus their per-op dispatch
tax; the island side is its registers, both instruction lists, and the
same tax again for each of the ten CALLs -- a CALL runs the graph's
own kernel, so absorbing one buys continuity, never speed. 658 < 669:
at this size the island is a wash, so the ops stay. (The listings
above were produced with `STANLI_ISLAND_ALWAYS=1`, the switch that
exists for exactly this question.) Scale the same shape up -- more
iterations, a wider state, real HMM emissions -- and the ratio flips,
which is the corpus table in [benchmarks.md](benchmarks.md).

## The receipts

Same point, three configurations -- islands off, islands forced,
default -- and one line of output each:

```
OK -10.372086118363603 11.984608360112961 -5.5685067343452275
OK -10.372086118363603 11.984608360112961 -5.5685067343452275
OK -10.372086118363603 11.984608360112961 -5.5685067343452275
```

The log density and both gradients agree to the last digit whether the
loop ran as 108 graph ops, as one island under the generated backward,
or however the estimate chose. That is the invariant every layer above
is built to preserve.
