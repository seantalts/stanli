# ODE sensitivity ceilings after Phase 1

**Status:** developer-only attribution and ceiling experiment complete on
2026-08-28. No experimental solver path is installed in the runtime.

The recommended branchless RK path was subsequently implemented and admitted;
see `2026-08-29-ode-direct-rk-results.md`. The CVODES experiments in this
report remain developer-only.

## Outcome

The merged Phase 1 implementation is a sound baseline, and both solver
families retain substantial headroom.

1. A stanli-owned all-double coupled RK solve, using the pinned Boost RK45 or
   Cash--Karp tableau directly, is an exact and compelling follow-up for
   branchless compiled RHS programs. Complete-solve geometric-mean speedups
   are `1.800x` on Lotka--Volterra and `1.796x` on soil under RK45. CKRK gives
   `1.821x` and `1.860x`. Values, every solution-Jacobian bit, and callback
   counts match the current Stan Math oracle; candidate step totals are
   recorded separately.
2. A three-operation CVODES derivative-provider seam is technically small
   and worthwhile for narrow active widths. On the real branched
   `one_comp_mm_elim_abs` RHS, the bounded BDF prototype is `1.210x` faster at
   complete-solve level and the forced Adams arm is `1.144x` faster.
3. The CVODES timing arm is not production-admissible yet. Its branch-aware
   `fvar<double>` Jacobian follows `JZ/JMP`, but arithmetic grouping differs
   from current reverse mode. Complete one-compartment errors remain tiny
   (`4.87e-14` worst absolute Jacobian error over three points), yet they are
   not bitwise exact.
4. One forward tangent replay per input direction is not a general strategy.
   A retained BDF confirmation loses at active width 16 (`0.857x`), and the
   screen collapses at width 64; the same mechanism loses on RK branches even
   at the small baseline shape (`0.773x`). The production
   follow-up needs an exact executed-instruction trace/reverse provider, not a
   general fvar-column path.
5. The failed Phase 0 generated-reverse/precomputed-gradient callback bridge
   remains stopped. None of these prototypes constructs precomputed-gradient
   callback nodes or installs that path. The RK experiment reuses the raw
   register adjoint only as a double-space `J_y/J_theta` provider inside a
   separately owned coupled solve.

Recommended PR sequence:

- pursue an exact, branchless RK45/CKRK coupled-sensitivity path first;
- build an exact branch-aware register derivative provider next;
- then expose the three-operation provider policy at the pinned CVODES seam;
- do not merge either current benchmark implementation as production code.

## Scope and terminology

The candidate/oracle ratios in this report are complete ODE solve ratios. They
include solver setup, integration, output materialization, and dense solution
Jacobian extraction. They do not include the surrounding graph or the final
`OP_ODE` matrix-vector pullback. The reported full-gradient numbers are
Amdahl projections from independently measured OP shares, not integrated
candidate timings.

The direct RK arm is deliberately a hard ceiling. It mirrors the pinned Stan
Math loop's Boost tableau, controller, full-coupled-state error norm, initial
step, observation schedule, tolerances, and step checker, but bypasses the
ordinary all-double RHS container adapter and repeated high-level validation.
A production implementation must preserve error semantics without putting
those checks back in the callback loop.

The CVODES arm is a proximity ceiling. It reproduces the pinned lifecycle and
official CVODES configuration, but uses fvar Jacobian columns whose arithmetic
grouping is not an exact replacement for the oracle.

All decision ratios below come from uninstrumented, sequential runs with no
other benchmark process active. The runner retains every batch, computes the
geometric mean and two-sided Student-t 95% interval in log-ratio space, and
writes a provenance sidecar. Earlier overlapping exploratory runs are retained
only as discarded diagnostics and are not quoted here.

## Provenance

- Base and `origin/main`:
  `0c89544b695e22eaf788eb88aa8622377e8921fc` (`Speed up builds and CI
  (#244)`), a descendant of `bdfc9e2867a9f2561b7890d19200e0676f020015`,
  which merged PR #245. All decision runs were repeated after the fast-forward.
- Candidate commit: none; all artifacts are uncommitted developer tools and
  this report.
- Host: Apple M3 Ultra, 32 cores, 96 GiB RAM.
- OS: macOS 26.4 (`25E246`), arm64.
- Compiler: Apple clang 21.0.0.
- Build: Release, `-O3 -DNDEBUG -ffp-contract=off`.
- CMake: 4.3.3.
- Stan Math: `8f326d14599d3030c626c46532d8e8534c1cdbec`.
- Stan: `c96d04115d35cb04f42e45c5a69a82f9704798f1`.
- stanc3: `5b824ee48c590fa229dcebf6b57457b2fd212aa8`
  (`v2.39.0-142-g5b824ee`).
- PosteriorDB: `28f8d3d6e975315f42aa274a8399f21e07a43b30`.
- CmdStan: `11cb052d3e1fc8c799e0fec559e2ee5452b38d27`.

The tools compile against the merged runtime, retain that solve as their
oracle, and do not modify `runtime/` or `deps/`. Their CMake targets are
`EXCLUDE_FROM_ALL`, so normal Release builds neither compile nor link the
diagnostic programs.

Every retained JSONL row includes SHA-256 hashes of its MIR, data, and selected
benchmark binary. Each run sidecar records the exact Git status, host, Python,
CMake cache, target compile flags/definitions, compiler versions, commands,
artifact hashes, and hashes for the uncommitted benchmark/generator sources.
The prepared manifests also record the source Stan/archive and pinned stanc
paths and hashes.

## Phase 1 baseline and whole-gradient attribution

Ten uninstrumented Release `bench_grad` runs gave these merged-baseline
medians:

| model | warmed gradient |
|---|---:|
| `lotka_volterra` | 40.477 us |
| `soil_incubation` | 56.084 us |
| `one_comp_mm_elim_abs` | 485.588 us |

Existing executor profiling separates the graph-level ODE forward and
backward work:

| model | OP_ODE forward / gradient | OP_ODE backward / gradient | profiled total | forward share | total OP share |
|---|---:|---:|---:|---:|---:|
| Lotka | 40.159 us | 0.143 us | 41.779 us | 96.12% | 96.47% |
| soil | 54.306 us | 0.118 us | 58.100 us | 93.47% | 93.67% |
| one-compartment | 483.939 us | 0.022 us | 484.614 us | 99.86% | 99.87% |

`OP_ODE` backward is therefore not a useful optimization target: it costs
roughly 22--143 ns per gradient. The work is in forward sensitivity solving.

The Phase 1 callback/marshalling result remains relevant context. Relative to
the pre-PR checkout, its direct Eigen output and legacy `_tol_impl` dispatch
improved complete gradients by `1.265x` on Lotka, `1.243x` on soil, and
`1.104x` on one-compartment. This experiment starts after those copies have
already been removed.

### Complete-solve work attribution

The standalone tools keep profiling as a separate template instantiation.
The paired arms contain no phase clocks, counters, statistics calls, or
runtime profiling branches. Component timers are perturbative and are used
only to locate work.

For one-compartment BDF at point 0, the oracle and candidate callback roles
match exactly:

| work | oracle | compiled CVODES arm |
|---|---:|---:|
| primal RHS calls | 1,066 | 1,066 |
| derivative callbacks | 572 | 9 `J_y` + 563 full `J_y/J_theta` |
| accepted steps | not externally exposed | 516 |
| CVODES primal RHS statistic | not externally exposed | 1,066 |
| CVODES Jacobian statistic | not externally exposed | 9 |
| CVODES sensitivity RHS statistic | not externally exposed | 563 |
| nonlinear iterations | not externally exposed | 539 |
| sensitivity nonlinear iterations | not externally exposed | 558 |

The corresponding Adams arm has 364 primal calls and 194 derivative calls on
both sides; the candidate reports 176 accepted steps, four state Jacobians,
and 190 sensitivity RHS evaluations.

A representative instrumented BDF candidate attributes approximately 63 us
to register seeding, 122 us to register-program bodies, 47 us to output
extraction, and 8 us to sensitivity products across the solve. These fields
overlap the callback totals and must not be summed. Constructor/allocation,
CVODES setup, `CVodeGetSens`, output packing, and final oracle harvest are each
low-single-digit microseconds or less. The dominant removable work is repeated
derivative construction inside the integration, not final Jacobian harvesting
or graph backward.

The candidate-only RK diagnostic shows the same shape. Times in the last five
columns are per callback and intentionally include clock perturbation:

| model | callbacks | instrumented solver | final packing | seed | body | output extraction | local Jacobian | sensitivity propagation |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Lotka | 260 | 61.2 us | 0.5 us | 16.2 ns | 16.7 ns | 13.2 ns | 55.1 ns | 19.3 ns |
| soil | 326 | 76.2 us | 0.5 us | 14.1 ns | 16.4 ns | 12.9 ns | 54.3 ns | 17.9 ns |

The oracle callback phase labels are not directly comparable: its register
timer excludes Stan Math's reverse sweeps and sensitivity products. Only the
uninstrumented complete-solve ratios are used for decisions.

The perturbative RK breakdown is retained separately in the diagnostic
runner artifact listed below; it is not inferred from the decision timings.

Allocation semantics are also bounded:

- the merged callback writes directly into caller-owned Eigen output;
- RK candidate register, value, Jacobian, and sensitivity buffers live for one
  solve and are reused by every callback;
- the direct RK callback fills Odeint-owned `std::vector<double>` storage and
  performs no vector-to-Eigen-to-vector coupled-state round trip;
- CVODES candidate state, sensitivity lanes, dense matrix, linear solver,
  `N_Vector` shells, register files, `J_y`, and `J_theta` are allocated once per
  solve;
- no tangent direction allocates inside a timed callback;
- normal Release runtime code contains none of this diagnostic machinery.

## RK45 and CKRK hard ceiling

### Prototype

The candidate state is

```text
z = [y,
     S_y0 lane 0, ..., S_y0 lane Ny-1,
     S_theta lane 0, ..., S_theta lane P-1]
```

with one contiguous state-sized block per active input. Its callback evaluates
compiled `f`, `J_y`, and `J_theta`, then uses the oracle's scalar order:

```text
y'          = f
S_y0'       = J_y * S_y0
S_theta'    = J_theta + J_y * S_theta
```

The complete coupled vector enters the adaptive error norm. A state-only solve
followed by a separate sensitivity integration is not used.

The raw generated register reverse is eligible only for instructions already
supported by `gen_adjoint`. It runs one double forward and one reverse sweep
per RHS output. It creates no `var`, `vari`, arena operand array, or
precomputed-gradient node. Unsupported control flow selects the fvar
experimental arm in this tool; production must instead select the current
oracle until an exact trace reverse exists.

### Real-model results

Each row uses 200 ms warmup per arm followed by 15 alternating paired batches;
each timed arm lasts at least 0.5 seconds per batch. The estimate is the
geometric mean of paired oracle/candidate ratios with a paired-log t interval.

| solver | model | oracle median | candidate median | speedup | paired 95% CI | callbacks | candidate accepted / attempted / rejected |
|---|---|---:|---:|---:|---:|---:|---:|
| RK45 | Lotka | 42.065 us | 23.396 us | **1.800x** | [1.769x, 1.830x] | 260 / 260 | 32 / 43 / 11 |
| RK45 | soil | 57.503 us | 32.196 us | **1.796x** | [1.772x, 1.820x] | 326 / 326 | 47 / 54 / 7 |
| CKRK | Lotka | 41.299 us | 22.569 us | **1.821x** | [1.803x, 1.839x] | 258 / 258 | 36 / 43 / 7 |
| CKRK | soil | 54.068 us | 28.958 us | **1.860x** | [1.849x, 1.871x] | 288 / 288 | 48 / 48 / 0 |

RK attempt counts are derived from the pinned six-stage tableau callback count;
accepted steps are returned by `integrate_times`. The derivation is reported
as such in tool output. The current high-level oracle does not expose its
accepted/rejected counters, so only total callback-work parity is asserted.

At point 0, Lotka compares all 40 solution values and 240 Jacobian entries;
soil compares 50 and 300. All bits match. Points 1 and 2 are also exact for
both tableaux:

| solver / model / point | callbacks | accepted steps | result |
|---|---:|---:|---|
| RK45 / Lotka / 1 | 152 / 152 | 23 | 40 / 40 values and 240 / 240 J bits exact |
| RK45 / Lotka / 2 | 134 / 134 | 22 | 40 / 40 values and 240 / 240 J bits exact |
| RK45 / soil / 1 | 302 / 302 | 44 | 50 / 50 values and 300 / 300 J bits exact |
| RK45 / soil / 2 | 308 / 308 | 45 | 50 / 50 values and 300 / 300 J bits exact |
| CKRK / Lotka / 1 | 132 / 132 | 22 | 40 / 40 values and 240 / 240 J bits exact |
| CKRK / Lotka / 2 | 132 / 132 | 22 | 40 / 40 values and 240 / 240 J bits exact |
| CKRK / soil / 1 | 258 / 258 | 43 | 50 / 50 values and 300 / 300 J bits exact |
| CKRK / soil / 2 | 258 / 258 | 43 | 50 / 50 values and 300 / 300 J bits exact |

Using only the separately measured `OP_ODE` forward shares, the RK45 solve
ceilings project to `1.745x` for the complete Lotka gradient and `1.707x` for
soil. CKRK projects to `1.765x` and `1.761x`. Transforming the solve intervals
through the same forward-only Amdahl calculation gives `[1.718x,1.773x]`,
`[1.687x,1.727x]`, `[1.748x,1.781x]`, and `[1.752x,1.771x]`, respectively.
`OP_ODE` backward and all other graph work remain unaccelerated in this
calculation. These are projections, not production A/B results.

## BDF and Adams CVODES proximity ceiling

### Smallest seam

Pinned Stan Math currently owns static callbacks and the `cvodes_mem` lifetime.
There is no standalone `J_theta` setter. The smallest useful policy is:

```text
rhs(t, y)               -> f
jacobian_states(t, y)   -> J_y
jacobians(t, y)         -> J_y, J_theta
```

The existing CVODES trampolines can use those operations for `CVRhsFn`,
`CVLsJacFn`, and `CVSensRhsFn`. The sensitivity callback computes
`J_y*S_y0` or `J_theta + J_y*S_theta` lane by lane. BDF and Adams share this
seam; the linear multistep method remains a CVODES choice.

The bounded local adapter preserves the pinned order:

1. create CVODES memory and initialize the primal state;
2. install user data before Jacobian and sensitivity callbacks;
3. apply Stan Math's max-step and tolerance configuration;
4. install the dense matrix and linear solver;
5. install `J_y`, then staggered sensitivity callbacks and sensitivity error
   control;
6. advance and harvest every requested output time;
7. query official statistics before `CVodeFree`;
8. destroy sensitivity shells, solver, matrix, state vector, and context in
   ownership order.

### Branch-aware one-compartment proof

The optimized one-compartment RHS is 19 instructions / 23 registers and has a
`JZ` around the dose arm. `run_program<fvar<double>>` evaluates comparisons on
the primal and differentiates only the selected arm.

At corpus point 0 and `-0.0`, `+0.0`, `nextafter(0,-infinity)`, and
`nextafter(0,+infinity)`, all four local `J_y/J_theta` entries and the RHS value
match reverse mode bit for bit. The positive adjacent value takes the dose arm
(`f = 16.577563771134717`); the other three return zero. This proves the actual
`t > 0` control flow is supported rather than structurally refused.

Fvar and reverse derivative grouping can still differ on the same selected
arm. At point 1, for example, one boundary-probe derivative differs by one ULP.
The three complete-solve checks are:

| solver | point | max value abs error | max full-J abs error | exact J entries |
|---|---:|---:|---:|---:|
| BDF | 0 | 1.78e-14 | 1.40e-14 | 20 / 80 |
| BDF | 1 | 1.78e-14 | 4.86e-14 | 21 / 80 |
| BDF | 2 | 2.49e-14 | 5.66e-15 | 21 / 80 |
| Adams | 0 | 1.60e-14 | 1.91e-14 | 21 / 80 |
| Adams | 1 | 7.11e-15 | 2.66e-14 | 27 / 80 |
| Adams | 2 | 1.07e-14 | 9.33e-15 | 23 / 80 |

Callback-role counts match at all timed points. Comparable internal CVODES
statistics are unavailable from the current oracle because pinned Stan Math
frees its private memory before returning; the tool explicitly reports that
solver-work parity is unknown beyond callback counts.

### Real-model timing

| solver | oracle median | candidate median | speedup | paired 95% CI | range |
|---|---:|---:|---:|---:|---:|
| BDF | 517.760 us | 418.811 us | **1.210x** | [1.195x, 1.225x] | 1.152x--1.248x |
| Adams | 241.633 us | 214.405 us | **1.144x** | [1.125x, 1.163x] | 1.098x--1.231x |

The forward-only BDF ceiling projects to `1.210x` for the complete
one-compartment gradient with a transformed interval of `[1.195x,1.225x]`.
The forced Adams result projects to `1.144x` with `[1.125x,1.163x]`. Neither
projection admits the current fvar provider.

## Synthetic shape sweep

`gen_ode_ceiling_sweep.py` emits 21 runnable cases for each solver, 84 total.
The one-factor axes around
`(S=2,P=4,T=8,branch=false,y0=active,theta=active)` are:

- `S = 1, 2, 4, 8, 16`;
- active theta width `P = 0, 1, 4, 16, 64`;
- output-time count `T = 1, 4, 8, 32, 128`;
- branch absent/present;
- initial state data/active;
- theta data/active;
- five interaction/edge cases.

The runnable matrix covers type masks `0x3` (active `y0` and theta), `0x2`
(data `y0`, active theta), and `0x1` (active `y0`, data theta). Four additional
genuine `0x0` models are recorded in `compile_only.json`. The optimized MIR
still contains those calls; Stanli's constant folder evaluates them outside
the repeated log-density graph. There is consequently no runtime
`OP_ODE` to benchmark. This documents the fourth activity combination without
faking activity merely to keep an operation alive. All four compiled inputs
were checked directly; both ceiling binaries report `compiled model has no
OP_ODE` as expected.

Every `S > 1` RHS has dense state coupling through
`0.02 * sum(y) / rows(y)`. Its off-diagonal `J_y` entries are `0.02 / S`, so
the sweep exercises a non-diagonal multi-state CVODES Jacobian rather than a
collection of independent scalar equations.

Every case runs in a fresh process. The retained screen uses three paired
batches of one solve with no duration warmup; it locates structural crossovers
and is not an admission benchmark. Ratios below are the median and range of
the 21 per-case paired geometric means.

| solver | median case ratio | range | correctness |
|---|---:|---:|---|
| RK45 | 1.577x | 0.156x--3.420x | 3,075 / 3,075 values, 54,693 / 54,693 solution-J entries, and 4,740 / 4,740 local entries exact; all callback counts equal |
| CKRK | 1.533x | 0.143x--3.363x | 3,075 / 3,075 values, 54,693 / 54,693 solution-J entries, and 4,740 / 4,740 local entries exact; all callback counts equal |
| BDF | 1.167x | 0.637x--1.559x | 3,075 / 3,075 values and 54,693 / 54,693 J entries exact |
| Adams | 1.188x | 0.481x--1.544x | 3,075 / 3,075 values and 54,693 / 54,693 J entries exact |

The synthetic algebra happens to be grouping-stable, so CV fvar is exact
there. Every synthetic row explicitly requests and passes the bitwise timing
gate; none is admitted under the proximity rule. That does not override the
real one-compartment counterexample.

Structural findings:

- generated-reverse RK wins all 18 branchless cases for both tableaux;
- the three currently fvar-backed RK branch cases lose for both tableaux;
- CV fvar wins 15 / 18 branchless shapes and 1 / 3 branch shapes for both BDF
  and Adams in the low-iteration screen;
- CV fvar is profitable at narrow `P`, loses in the retained `P=16`
  confirmation, and loses decisively at `P=64` in the screen;
- data `y0` and data theta exercise different coupled widths, so eligibility
  cannot be inferred from the source parameter count alone;
- the exact synthetic CV results demonstrate dense-`J_y` mechanics, but do
  not override the real one-compartment fvar/reverse grouping counterexample.

Long confirmation runs with the admission warmup/sampling protocol show:

| solver / shape | provider | speedup | paired 95% CI | verdict |
|---|---|---:|---:|---|
| RK45 `S16/P4/T8`, straight | generated reverse | 1.253x | [1.239x, 1.267x] | bitwise-exact win |
| RK45 `S2/P4/T8`, branch | fvar columns | 0.773x | [0.765x, 0.782x] | bitwise exact here, reject on cost |
| BDF `S2/P16/T8`, straight | fvar columns | 0.857x | [0.843x, 0.871x] | bitwise exact here, reject on cost |
| BDF `S2/P4/T8`, branch | fvar columns | 1.123x | [1.111x, 1.135x] | bitwise exact here; proximity algorithm only |

All four confirmation rows use 200 ms warmup and 15 paired batches; the
shortest timed arm in any batch is 0.542 seconds.

These rows show why eligibility cannot be only “compiled RHS available.” The
derivative algorithm and active width matter.

## Recommendation matrix

| family / proposed path | evidence | production prerequisite | decision |
|---|---|---|---|
| RK45/CKRK, branchless direct coupled solve | exact 1.80x--1.86x real-model solve ceilings; all 18 branchless synthetic cases win | retain Phase 1 `run_rhs_into` seam, immutable generated derivative payload, exact checks, fallback selector, error-semantic tests | **Follow-up PR merits immediate work** |
| RK45/CKRK, current fvar branch path | 0.773x at the small branch; worse when wide | exact executed-trace reverse or another reverse-cost provider | **Stop; do not integrate** |
| BDF/Adams, three-operation CVODES provider seam | real P=3 branch wins 1.210x / 1.144x; P=4 synthetic branch wins 1.123x BDF | exact branch-aware reverse provider; oracle CVODES work-stat hook; same-binary full-gradient A/B | **Follow-up seam/prototype merits work** |
| BDF/Adams, fvar column provider | loses by P=16 and is non-bitwise on one-compartment | none; replace the algorithm | **Do not ship** |
| Phase 0 precomputed-gradient callback bridge | mandatory gate failed at 0.853x / 0.930x | material premise change | **Remain stopped** |

The first RK PR should be deliberately narrow:

- support only branchless compiled programs whose generated local values and
  full Jacobian are bitwise exact;
- retain the exact current solve as a runtime-selectable oracle/fallback;
- preserve the complete coupled error norm and tableau;
- preserve high-level input and error semantics outside the callback loop;
- compare accepted/attempted/rejected steps and callback counts;
- use PR #245's direct-Eigen `run_rhs_into` structure rather than restoring a
  vector-returning adapter;
- measure integrated `bench_grad`, default CmdStan, and `stanc --O1` CmdStan
  before enabling by default.

The CVODES follow-up should begin with the derivative payload, not a copied
solver:

- generate an executed-instruction trace that records the chosen `JZ/JMP`
  path and applies reverse rules in the oracle's grouping;
- validate signed zero, adjacent branch values, nonfinite behavior, and every
  supported opcode;
- expose `f`, `J_y`, and `J_y/J_theta` behind the smallest policy extension
  possible;
- if an upstream/pinned-header policy is unavailable, keep a repository-owned
  lifecycle adapter bounded to the audited initialization order;
- select by measured structural cost, with the current path as fallback.

## Correctness and admission gates still required

Before any production integration:

1. require bitwise local RHS and complete `J_y/J_theta` equality for every
   selected derivative program;
2. reject nonfinite comparison inputs even when both sides have the same bit
   pattern; both benchmark gates now do this before any timing begins. The CV
   proximity gate also requires each finite difference to meet either
   `1e-12` absolute or `1e-9` relative tolerance, while grouping-stable
   synthetic rows explicitly require bitwise equality;
3. require same-binary complete solution values, full scratch Jacobian,
   callback counts, and RK step history;
4. expose or copy diagnostic CVODES glue so oracle/candidate step, nonlinear,
   Jacobian, and sensitivity statistics can be compared;
5. run all three deterministic corpus points, the complete 84-case runtime
   matrix, and confirm the four compiler-eliminated data/data dispositions;
6. run the merged CmdStan differential oracle and error-behavior tests;
7. run 200 ms warmup per arm and at least 15 paired batches of at least 0.5 s
   per arm with profiling compiled out;
8. run integrated complete-gradient A/B measurements rather than admitting
   from the projections in this report;
9. confirm on Linux x86-64 as well as macOS arm64.

The runner additionally refuses input/output path collisions, rehashes MIR,
data, and the selected binary before and after every case, requires the
explicit pre-timing gate, and rejects missing, duplicate, nonfinite, or
nonpositive batch samples rather than computing a partial interval.

The merged Phase 1 baseline already has three-point CmdStan parity: 94 values,
worst relative error `1.57e-13`, plus six discriminating legacy solver cases.
On the final `0c89544` base, the broader repository sweep also passed all 16
ODE interfaces against CmdStan, covering every modern default/tolerance
solver, vector and mixed arguments, and all six legacy cases; its worst
reported relative error was `2.40e-14`. This experiment changes no runtime
code, so those oracles remain applicable.

## Artifacts and reproduction

Repository artifacts:

- `tools/bench_ode_ceiling.cpp`: exact oracle plus direct all-double RK45/CKRK
  ceiling, local/full checks, step/callback counts, diagnostic attribution, and
  paired timing;
- `tools/bench_ode_cvodes_ceiling.cpp`: exact oracle plus local BDF/Adams
  CVODES proximity arm, branch proof, official statistics, attribution, and
  paired timing;
- `tools/gen_ode_ceiling_sweep.py`: 84-case modern-interface generator plus
  four genuine compiler-eliminated data/data dispositions;
- `tools/prepare_ode_ceiling_models.py`: reproducible pinned PosteriorDB/stanc
  preparation of all three real inputs and six solver rows;
- `tools/run_ode_ceiling_sweep.py`: fresh-process runner, exact case filters,
  parser, JSONL/raw-log writer, paired-log statistics, hashes, provenance,
  safe owned replacement, and timeout/failure continuation;
- `tools/bench_rhs_adjoint.cpp`: retained failed Phase 0 callback benchmark;
- `docs/superpowers/plans/2026-08-28-ode-rhs-generated-adjoint-results.md`:
  Phase 0 stop report.

Ephemeral retained artifacts for this run:

- `/tmp/stanli_ode_ceiling_final_0c89544/manifest.json`: reproducibly prepared real
  inputs and six solver rows;
- `/tmp/stanli_ode_ceiling_final_0c89544/real_rk_15x26000_final_0c89544.jsonl`,
  `real_bdf_15x1300_final_0c89544.jsonl`, and
  `real_adams_15x2500_final_0c89544.jsonl`: idle sequential decision runs;
- `/tmp/stanli_ode_ceiling_final_0c89544/parity_rk_point{1,2}_1x3_final_0c89544.jsonl`
  and `parity_cv_point{1,2}_1x3_final_0c89544.jsonl`: retained nonzero-point checks;
- `/tmp/stanli_ode_ceiling_final_0c89544/diagnostic_rk_1x1_final_0c89544.jsonl`:
  retained perturbative RK attribution and raw logs;
- `/tmp/stanli_ode_synthetic_v6_0c89544/manifest.json` and
  `compile_only.json`: 84 runnable rows and four all-data dispositions;
- `/tmp/stanli_ode_synthetic_v6_0c89544/screen_1x3_final_0c89544.jsonl`: complete
  screening results;
- `/tmp/stanli_ode_synthetic_v6_0c89544/long_*_final_0c89544.jsonl`: four
  sequential long confirmations;
- every JSONL above has a sibling `.logs/` directory and
  `.provenance.json` sidecar with raw output and reproduction metadata.

Representative commands:

```sh
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel -j8 \
  --target bench_rhs_adjoint bench_ode_ceiling bench_ode_cvodes_ceiling

python3 tools/prepare_ode_ceiling_models.py \
  /tmp/stanli_ode_ceiling_final_0c89544

python3 tools/run_ode_ceiling_sweep.py \
  /tmp/stanli_ode_ceiling_final_0c89544/manifest.json \
  --output /tmp/stanli_ode_ceiling_final_0c89544/real_rk.jsonl \
  --solvers rk45,ckrk --iterations 26000 --batches 15 --warmup-ms 200

python3 tools/gen_ode_ceiling_sweep.py \
  /tmp/stanli_ode_synthetic_v6_0c89544 \
  --stanc deps/stanc3/stanc

python3 tools/run_ode_ceiling_sweep.py \
  /tmp/stanli_ode_synthetic_v6_0c89544/manifest.json \
  --output /tmp/stanli_ode_synthetic_v6_0c89544/screen.jsonl \
  --iterations 1 --batches 3 --warmup-ms 0
```

## Final boundary

The result supports owning coupled sensitivity execution where the evidence is
exact and large enough. It does not support installing a general derivative
provider merely because it avoids autodiff.

The next production experiment should be the exact branchless RK path. In
parallel, the useful BDF/Adams research task is an exact branch-aware trace
reverse plus the three-operation CVODES seam. The Phase 0 precomputed callback
bridge and the current fvar-column production path should remain out.
