# How stanli works, and why an interpreter can outrun a compiler

stanli runs Stan models with no C++ compiler on your machine, and on
many models it evaluates gradients faster than the natively compiled
model CmdStan produces. Both halves of that sentence sound wrong. This
document explains why they are not.

## What "compiling a model" actually does

When CmdStan builds your model, `stanc` translates it into a C++ file,
and a C++ compiler spends several seconds turning that into machine
code. But the generated file contains less than you might think. Your
model block

```stan
y ~ normal(mu + tau * theta_tilde, sigma);
```

becomes, in essence,

```cpp
lp += normal_lpdf(y, mu + tau * theta_tilde, sigma);
```

where `normal_lpdf` and everything else your model touches are calls
into stan-math, a library that could have been compiled long before
your model existed. The generated code contributes no new math, only an
*arrangement*: which library operations to call, in what order, wired
to which variables.

That is the whole trick. The vocabulary of operations is closed, because
it is exactly the Stan function library. New models do not add
operations; they arrange existing ones. And an arrangement does not
need a compiler. An arrangement is data.

## The op graph

stanli ships every operation precompiled inside one shared library and
turns your model into a flat list of operations, each an opcode plus
indexes into a preallocated array of doubles:

```
op 0: MUL          tau, theta_tilde   -> t0        (vector scale)
op 1: ADD          mu, t0             -> theta     (broadcast add)
op 2: NORMAL_LPDF  y, theta, sigma    -> target    (summed vector density)
```

Evaluating the log density means walking the list and calling each
opcode's precompiled function. No machine code is specific to your
model, in the same way a spreadsheet does not recompile Excel when you
type a new formula. Two things make this more than a toy:

- **The compiler is the real compiler.** The official `stanc` (OCaml)
  is linked into the library and runs in-process, so your model is
  parsed, typechecked, and optimized by the same code CmdStan uses.
- **The kernels are the real kernels.** The precompiled operations are
  stan-math, the same library CmdStan calls, compiled with the same
  floating point settings. That is why stanli agrees with CmdStan to
  the last bit on 45 of the 120 posteriordb test models, and to within
  2.6e-12 relative on the worst.

Because nothing is compiled at model load, "compiling" takes
milliseconds, and the time from `model.stan` to a first draw is about
twenty times shorter than CmdStan's.

## Gradients without code generation

Stan gets gradients from reverse-mode automatic differentiation: run
the computation forward, remember what you did, walk the record (the
"tape") backward applying the chain rule. In CmdStan the tape is built
dynamically: every scalar operation on a parameter creates a small heap
object with a virtual method, and each gradient evaluation builds the
tape, walks it, and tears it down. HMC evaluates the gradient once per
leapfrog step, thousands of times per run.

stanli does not build a tape, because it already has one. The op list
*is* the record of the computation: to differentiate, walk it backward
and call each opcode's derivative function. The tape is constructed
once, at model load, and steady-state gradient evaluation allocates no
memory at all.

## How an interpreter can win

Interpreters have a fixed overhead, some tens of nanoseconds per
operation dispatched. The question is what that overhead is amortized
over.

**stanli pays per operation. CmdStan pays per scalar.** Executing
`NORMAL_LPDF` over a thousand observations, stanli pays its dispatch
overhead once, then runs precompiled vectorized code over a flat array;
divided by a thousand elements the overhead vanishes, the same reason
vectorized R and NumPy are fast. CmdStan has no dispatch overhead, but
each observation allocates a tape node, and the backward pass walks a
thousand heap objects through virtual calls, every leapfrog step. Its
autodiff matrices also store pointers to tape nodes, so values sit
scattered in memory, which defeats SIMD; stanli's values live in flat
contiguous arrays. On vectorized models both effects point the same
way, and stanli evaluates gradients two to six times faster, with the
gap growing with data size.

**The graph persists, so it can be optimized.** CmdStan's tape lives
for one gradient evaluation; stanli's graph is built once and reused
millions of times, so it is worth running compiler-style passes over
it. A model written as an explicit loop,

```stan
for (n in 1:N)
  y[n] ~ normal(alpha[county[n]], sigma);
```

lowers to N copies of a few scalar operations, where the interpreter
overhead really does hurt. But the copies sit in a list, visibly
periodic, and a pass rewrites them: gathers become one vector index
operation, N scalar densities become one summed vector density. One
radon model drops from 27,670 operations to 8 and goes from slightly
losing against CmdStan to beating it six-fold. The statistician did not
vectorize the model; the runtime did, once, at load time. You can only
do that to a computation that exists as an inspectable data structure.

## The three machines inside the runtime

Everything above describes the main engine, but the runtime actually
contains three, and the honest way to see why is one question: **how
much do you know before the model runs?** The more you decide early,
the faster you go. The less you assume, the more you can handle. Each
engine sits at a different point on that line.

**The MIR interpreter decides everything late.** It walks the
compiler's syntax tree and looks variables up by name, every time.
That is slow, and that is fine, because its jobs are the ones where
speed cannot matter or early decisions are impossible: transformed
data runs once at load; generated quantities can call a random number
generator and branch on the draw, so there is nothing fixed to
compile; and it is the safety net for anything the fast paths refuse.
That last job carries a rule this codebase treats as law: **the
fallback must always speak more of the language than the fast path**,
because a refusal has to land somewhere that still works. If the
fallback were ever narrower, a refusal would become a crash.

**The op graph decides shapes and order early.** That is the engine
this document is about: the fixed list is the tape, and vector-sized
ops amortize the dispatch cost. Its two hard walls are structural. A
fixed list cannot contain a decision, so `if (theta > 0)` -- which
picks its branch anew every evaluation -- cannot become ops at all.
And when a loop cannot be vectorized because step t needs step t-1's
answer, the list holds thousands of one-number ops, each paying the
per-op overhead that vector ops exist to amortize.

**The register machine also decides placement early.** Regions that
hit the graph's walls -- a recurrence, a branch on a parameter, an ODE
right-hand side the solver calls at times of its choosing -- compile
one level further down, into a flat instruction list over numbered
cells fixed at load time. No slot lookup, no descriptor: an
instruction costs about a nanosecond. It may branch, because it runs
entirely inside one graph op, so from the tape's point of view the
branch never happened -- the same way a branch inside any kernel is
invisible. Its gradient is a second instruction list, generated at
compile time by reading the first one backward and replacing each
instruction with its derivative rule, running over a parallel array of
adjoints. Both lists are plain doubles; nothing allocates.

Where the mathematics lives differs by class, deliberately. The scalar
continuous densities -- the `normal_lpdf` in an HMM's emission, the
hottest instructions an island has -- go through one shared table that
binds the arguments to a recording scalar and calls the unmodified
Stan library, which computes the value and the partial derivatives
itself. Everything else with a one-number result (a cdf, `pow`, a
discrete density) is a `CALL`: the register machine invokes the graph
op's own kernel, the identical code, partials, and backward the graph
would have run. One instruction, rather than hundreds of ported rules,
is what keeps the three vocabularies from drifting: the densities are
special only in being hot enough to deserve a direct path, and narrow
enough (plain scalar in, one scalar out) that the direct path is a
single library call. Only the elementary arithmetic -- multiply,
divide, `exp`, a couple dozen more -- has hand-written derivative
rules, each transcribed from the Stan library's own reverse-mode
source and pinned by tests that require bit-for-bit agreement with it.

One mechanism ties the engines' gradients together: scratch. Every
graph op may declare working space; at model load the executor sums
the requests and hands each op a fixed slice of one flat arena. An
op's forward stashes its partial derivatives there and its backward
reads them, which is why a gradient allocates nothing and why the
backward never needs to re-run the forward. An island is the same
idea one level down: its scratch slice holds its whole register file,
so the values its forward computed are still sitting there when its
backward runs, and a CALL's kernel scratch is carved out of that same
file. It is scratch all the way down, allocated once, addressed by
offsets decided before the first gradient ever runs.

All of this is concrete in
[lowering-walkthrough.md](lowering-walkthrough.md): a tutorial that
takes three ten-line models -- the normal vectorized path, a branch on
a parameter, a recurrence -- and shows the real output of every layer,
from the compiler's tree to the generated backward.

## Where the compiled model still wins

ODE models run at about 0.6x: the right-hand side must stay callable
at solver-chosen times, and stanli runs it through the register
machine where CmdStan runs native code. Sequential models used to
lose the same way, until the islands' backward stopped replaying under
CmdStan-style autodiff and became a generated program: the HMM family
now measures 1.4-1.6x against islands off, and `iohmm_reg` 4.7x
(docs/benchmarks.md has the table). What remains for both classes is
the per-instruction cost of interpreting at all, which is the
difference between a nanosecond of dispatch and code the C++ compiler
inlined into the model binary.

The other cost is size, the deliberate trade at the center of the
design: the wheel is 7.8 MB because it carries the entire Stan compiler
and every operation in the math library, whether your model uses them
or not. In exchange, `pip install stanli` is the entire installation.

## The one-paragraph version

Stan models are arrangements of a fixed library of operations, and an
arrangement is data, not code. Ship the library precompiled, turn the
model into a graph of operations over flat arrays, and the graph is
simultaneously the program, the autodiff tape, and an optimizable
artifact. Interpreting at vector granularity costs almost nothing;
never rebuilding the tape saves what CmdStan spends every leapfrog
step; and optimizing the persistent graph recovers the vectorization
that loops in the source hide. That is how there is no compiler, and
why its absence is sometimes a speedup.
