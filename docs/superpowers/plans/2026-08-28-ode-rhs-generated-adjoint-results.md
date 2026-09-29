# ODE RHS generated-adjoint experiment: Phase 0 result

## Outcome

**STOP after Phase 0. Do not build or ship the production feature.**

The best compliant generated-callback prototype is slower than the current
`run_rhs<stan::math::var>` callback on both eligible real RK45 targets:

| stanc-generated optimized MIR | old median | generated median | paired old/generated |
|---|---:|---:|---:|
| `lotka_volterra::dz_dt` | 94.1 ns | 110.8 ns | 0.853x |
| `soil_incubation::two_pool_feedback` | 109.6 ns | 118.0 ns | 0.930x |

Seven independent Release runs per target, each containing 51 alternating
paired batches of 100,000 callbacks, put the Lotka ratios between 0.834x and
0.855x and the soil ratios between 0.916x and 0.935x. The generated path was
about 17% to 20% slower on Lotka and 7% to 9% slower on soil. The plan required
at least 2.0x at this gate, so production refactoring, integration, corpus
timing, and a PR are intentionally not attempted.

This is the result the plan's stop rule is for: the source transformation is
viable, but the complete callback lifecycle does not leave enough work to
amortize generated reverse rows and precomputed-gradient nodes on a small ODE.

## Provenance

- Base: `a018ab76b378d587211ac746a75a86684f72a232`, latest `origin/main` when
  the experiment began, merging PR #243.
- Candidate commit: none. The experiment remains an uncommitted developer
  benchmark and report.
- Host: Apple M3 Ultra, 32 physical/logical cores, 96 GiB RAM.
- OS: macOS 26.4 (`25E246`), arm64.
- Compiler: Apple clang 21.0.0, `-ffp-contract=off` through the project target.
- CMake: 4.3.3.
- Stan Math: `8f326d14599d3030c626c46532d8e8534c1cdbec`.
- Stan: `c96d04115d35cb04f42e45c5a69a82f9704798f1`.
- stanc3: `5b824ee48c590fa229dcebf6b57457b2fd212aa8`
  (`v2.39.0-142-g5b824ee`).
- PosteriorDB: `28f8d3d6e975315f42aa274a8399f21e07a43b30`.
- CmdStan: `11cb052d3e1fc8c799e0fec559e2ee5452b38d27`
  (v2.39.0, TBB built).

The pinned corpus setup completed and an immediate second invocation was
idempotent.

## What Phase 0 measured

`tools/bench_rhs_adjoint.cpp` accepts any stanc-generated optimized TMIR
function and uses its finalized, post-compaction `RhsProgram`. It was first
validated on `tests/fixtures/odefns.tmir.sexp::f_lin`, then measured on the
pinned PosteriorDB/CmdStan models' actual optimized MIR. The Lotka RHS is:

```text
du = (theta[1] - theta[2] * v) * u
dv = (-theta[3] + theta[4] * u) * v
```

The old arm measures the actual callback lifecycle:

1. Open the per-callback nested reverse-mode scope.
2. Construct the evolving state vars.
3. Execute the canonical compacted program on vars.
4. Sweep RHS outputs in ascending order.
5. Harvest state and persistent deep-copied-theta adjoints.
6. Clear nested adjoints between rows and theta adjoints after every row.

The generated arm uses only existing production primitives and measures:

1. Copy the canonical `Program` into an `IslandProg` prototype.
2. Mark time and `x_r` inactive, state and theta active.
3. Generate the checkpointed forward and reverse with existing
   `gen_adjoint`.
4. Clear and seed a reusable double register file; execute one double forward.
5. Clear the complete adjoint file for each output, seed through `adj_reg`,
   execute `run_adjoint`, and harvest a row.
6. Allocate one shared operand-pointer array and one contiguous Jacobian on
   Stan Math's current arena.
7. Construct each output with the low-level
   `precomputed_gradients_vari(value, size, operands, row)` constructor.
8. Perform the same ascending Stan sweeps and harvests as the old arm.

The active theta vars are created outside each callback's nested scope, as
`coupled_ode_system` does with `deep_copy_vars`. Only the evolving state and
RHS graph/nodes are per-callback. The benchmark alternates arm order, warms
both paths, consumes values and every harvested derivative through a volatile
sink, and performs no ordinary heap allocation after workspace warm-up except
the returned/output containers already present in the callback contract.

The actual Lotka RHS has 6 instructions and 13 registers; the generated
sibling has 6 forward and 6 reverse instructions. The actual soil RHS has 8
instructions and 15 registers; its sibling has 8 forward and 8 reverse
instructions. Neither needs checkpoints. These are favorable cases for
generated adjoints: straight-line scalar arithmetic, no branch refusal, no
checkpoint expansion, and only two output rows.

## Correctness checks performed before timing

The prototype requires bitwise equality for:

- RHS output values.
- Every generated local Jacobian entry versus the current var replay.
- Every derivative harvested after the precomputed-gradient output sweeps.
- A second consecutive callback through each arm.

It also verifies that persistent theta adjoints return to positive zero after
each callback. The focused clean-baseline test set passed before the spike:

```text
test_adjoint
test_ode_prog
test_odevariadic
test_algebra
test_mir_unary_fallback
test_cross_path
```

After adding the developer benchmark, the same focused set was rebuilt and
remained green. No production runtime behavior changed.

## Repeated timing results

Command:

```sh
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j8 --target bench_rhs_adjoint
./build-rel/bench_rhs_adjoint 100000 51 /tmp/stanli_rhs_bench.Zo4JJw/lotka_volterra.tmir.sexp dz_dt 2 4 0
./build-rel/bench_rhs_adjoint 100000 51 /tmp/stanli_rhs_bench.Zo4JJw/soil_incubation.tmir.sexp two_pool_feedback 2 4 0
```

Lotka--Volterra:

| run | old var ns | generated ns | paired old/generated |
|---:|---:|---:|---:|
| 1 | 94.1 | 113.1 | 0.834x |
| 2 | 94.3 | 110.7 | 0.854x |
| 3 | 93.8 | 111.7 | 0.843x |
| 4 | 93.8 | 111.5 | 0.838x |
| 5 | 94.7 | 110.8 | 0.853x |
| 6 | 94.1 | 110.6 | 0.855x |
| 7 | 94.6 | 110.4 | 0.853x |
| median | **94.1** | **110.8** | **0.853x** |

Soil incubation:

| run | old var ns | generated ns | paired old/generated |
|---:|---:|---:|---:|
| 1 | 108.8 | 118.6 | 0.919x |
| 2 | 109.6 | 117.6 | 0.930x |
| 3 | 110.3 | 118.8 | 0.926x |
| 4 | 109.0 | 116.5 | 0.933x |
| 5 | 108.9 | 118.7 | 0.916x |
| 6 | 110.4 | 118.0 | 0.935x |
| 7 | 109.7 | 117.3 | 0.932x |
| median | **109.6** | **118.0** | **0.930x** |

As a supplementary check, five runs of the local `f_lin` fixture produced a
0.950x median ratio (range 0.939x--0.965x), also slower than var replay.

The result is not close to the 2.0x threshold; it is on the wrong side of
parity. Confidence intervals are unnecessary to classify this gate because
all fourteen real-model runs agree on the direction and the required effect
is far outside the observed noise bands.

## Gate evaluation

| required gate | result | decision |
|---|---|---|
| Complete generated callback at least 2x faster on representative eligible RK45 RHS | 0.853x on actual Lotka; 0.930x on actual soil | **FAIL** |
| Credible measured path to at least 10% whole-gradient improvement on Lotka | actual callback regresses by about 17% | **FAIL / not credible** |
| A second ODE has at least a 3% whole-gradient ceiling | not run after mandatory first-gate failure | **NOT REACHED** |

Per the original instructions, later phases stop here. In particular, there is
no default-on path, environment selector, cost model, sanitizer candidate,
CmdStan ODE sweep, full corpus performance run, or PR. Running those after the
mandatory callback gate failed would turn a bounded spike into the ODE
subsystem rewrite the plan explicitly forbids.

## Target eligibility at the stop point

The following records prototype generation/refusal, not a claim that a
production sibling was installed:

| target | solver/path | classification |
|---|---|---|
| `lotka_volterra::dz_dt` | RK45, two states, four theta | generated and verified bitwise; 6 forward/6 reverse instructions |
| `soil_incubation::two_pool_feedback` | RK45, two states, four theta | generated and verified bitwise; 8 forward/8 reverse instructions |
| `one_comp_mm_elim_abs` | BDF, one state, three theta, two `x_r` | structurally refused on `JZ`; 19 instructions/23 registers |
| `odevariadic` BDF fixture | CVODES/BDF | structurally eligible branchless fixture |

`one_comp_mm_elim_abs` would therefore retain the exact canonical var callback
even if later phases existed. The BDF audit also confirmed that CVODES invokes
both a var-state/data-theta state-Jacobian callback and, for active solves, the
coupled sensitivity callback. Selection must use actual callback scalar types,
not the original ODE op's activity mask.

## Independent Fable review

Claude Fable 5 reviewed the plan at high effort against this checkout before
the stop decision. Its verdict was that the architecture is feasible with
amendments, and its explicit stop criterion was: **stop on any Phase 0 miss**.
It recommended tightening any future revival in these ways:

- Require bitwise local-Jacobian equality and either bitwise forward-value
  parity or structural refusal for opcodes whose double and var reductions
  differ (`DOT` is already deliberately split).
- Zero the complete adjoint file per output and seed via `adj_reg`; use `+=`
  for the seed so an output aliased with a live-in retains its identity
  contribution.
- Generate against a copy and attach the immutable sibling only at ODE
  lowering, keeping algebra and the canonical fallback pristine.
- Read the environment gate once at lowering while still generating the
  sibling, so fresh-process A/B arms have identical preparation.
- Prove BDF activation for both theta-active and theta-inactive callback
  instantiations, not merely numerical agreement.
- Use a 2% investigation threshold and 3% hard regression stop consistently.

The Stan Math audit independently confirmed the core lifetime rule: output
nodes may share one arena-owned operand array and contiguous Jacobian, but they
must never retain pointers into reusable thread-local Jacobian storage.

A parallel ODE change has now completed explicit legacy RK45/BDF/Adams
dispatch and replaced `VarRhs`'s vector marshalling with `run_rhs_into` and
direct Eigen output. That work reports clean paired speedups of 1.265x on
Lotka, 1.243x on soil, and 1.104x on `one_comp`, with six live solver-dispatch
cases passing CmdStan. It has no overlapping hunks to reconcile with this
developer-only spike. If generated adjoints are ever reconsidered, their
selector must be a compiled-RHS helper callable from the direct `VarRhs` seam;
placing it only in the former vector-returning `MirRhs::eval` would be bypassed.
The remaining opportunity indicated by these combined results is BDF/CVODES
sensitivity cost, not the generated callback bridge measured here.

## Preparation, memory, and line counts

- Production preparation overhead: none (no production payload exists).
- Production retained memory: none.
- Prototype structural payload: copied 6-instruction/13-register and
  8-instruction/15-register forwards, with equal-length reverse programs, for
  the two measured cases.
- Production lines changed: 0.
- Runtime/test lines changed: 0.
- Developer benchmark: one 564-line standalone source file plus a three-line
  CMake target; it is not installed and is not a CTest test.

## Reconsideration boundary

Do not restart this design because the refactor is easy. Reconsider only if a
material premise changes, for example:

- Stan Math exposes a callback Jacobian interface that avoids constructing and
  sweeping precomputed-gradient nodes; or
- profiling finds a structurally different, common eligible RHS whose complete
  callback is at least 2x faster *and* a conservative structural rule excludes
  the small Lotka-shaped regression.

Any revival starts again at Phase 0 and must include the stricter parity and
CVODES activation checks above.
