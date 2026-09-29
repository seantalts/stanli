# ODEs beyond CmdStan: compiled sensitivity design

**Status:** correctness repair and Phase 1 implemented, 2026-08-28; raw parity
cleared on all three ODE models, but the 1.05x narrow gate still misses on the
one-compartment BDF model.

**Review basis:** source audit of stanli and its pinned Stan Math, the retained
corpus results, the 2026-08-24 allocation experiment, the unmerged 2026-08-27
selective-JIT measurements, and an independent Claude Fable 5 architecture
review.

## Implementation checkpoint

The correctness repair now maps legacy RK45, BDF, and Adams explicitly in both
graph lowering and write-array interpretation. A six-case live CmdStan sweep
(default and explicit tolerances for every solver, with write-array forced
through the interpreter) passes with 45 compared values per case and a worst
relative error of `2.40e-14`. The pre-fix binary fails the discriminating Adams
cases by `2.82e-6` and `2.96e-4`, so the oracle demonstrates the repaired bug
rather than merely exercising the code.

Phase 1 is also implemented. Fifteen paired Release rounds against a clean
pre-change checkout measured these complete-gradient speedups (geometric mean
and paired 95% confidence interval):

| Model | Phase 1 / pre-change speedup | 95% CI |
|---|---:|---:|
| `lotka_volterra` | 1.265x | [1.248x, 1.283x] |
| `soil_incubation` | 1.243x | [1.233x, 1.254x] |
| `one_comp_mm_elim_abs` | 1.104x | [1.071x, 1.138x] |

A fresh 15-round comparison used pinned CmdStan 2.39.0, Stan Math
`8f326d14`, Apple clang 21, and `-O3 -ffp-contract=off`. Ratios below are
CmdStan time divided by Phase 1 stanli time, so values above one favor stanli:

| Model | default CmdStan / stanli | 95% CI | `stanc --O1` CmdStan / stanli | 95% CI |
|---|---:|---:|---:|---:|
| `lotka_volterra` | 1.160x | [1.150x, 1.171x] | 1.142x | [1.126x, 1.158x] |
| `soil_incubation` | 1.176x | [1.163x, 1.189x] | 1.128x | [1.120x, 1.137x] |
| `one_comp_mm_elim_abs` | 1.022x | [1.013x, 1.031x] | 1.031x | [1.017x, 1.045x] |

The complete three-point oracle remains within `1e-9` of CmdStan for all three
models (94 values, worst relative error `1.57e-13`). Phase 1 therefore clears
its retention gate and clears raw parity everywhere, but only the two RK45
models clear the stricter 1.05x release target.

The generated-adjoint B0 proof was also run independently and failed its
mandatory admission gate: complete eligible callbacks slowed from 94.1 to
110.8 ns on Lotka and from 109.6 to 118.0 ns on soil; the one-compartment
program was correctly refused because it contains control flow. No production
generated-adjoint path was built. The next performance experiment should be
specific to the remaining BDF/CVODES sensitivity cost rather than adding that
bridge to the two models Phase 1 already carries comfortably past the target.

## Decision

Take a gated, three-step path rather than replacing an integrator up front:

1. remove stanli's callback marshalling and legacy-interface allocation tax;
2. provide exact compiled RHS values and Jacobians to the unchanged Stan Math
   solvers; and
3. own the coupled sensitivity callback only if the Stan Math bridge still
   leaves material overhead.

The first step is the smallest credible crossing experiment. The second is the
stanli-specific source of durable headroom: the RHS is already a register
program, and stanli already knows how to source-transform register programs
into double-space derivatives. CmdStan has to build and reverse a `var` tape at
each sensitivity callback; an eligible stanli RHS does not.

Do **not** pursue native/JIT dispatch as the ODE plan. The measured Lotka RHS is
only six instructions. An optimistic native helper experiment improved its
active `var` callback from 111.490 ns to 108.507 ns and projected to about 1.4%
of a complete gradient. That is below the present 10-15% gap before accounting
for runtime-code complexity.

## What "past parity" means

There are two separate bars.

**Narrow throughput bar.** The three currently measured ODE models must each
beat pinned default CmdStan by at least 5%, with a paired confidence interval
whose lower bound is above parity. CmdStan built from `stanc --O1` is reported
as a named secondary competitor; it must not be hidden by a default-only
headline.

**Broad parity bar.** Solver selection, supported argument shapes, active time
arguments, write-array execution, values, gradients, and error categories must
agree with CmdStan over the supported ODE surface. A speed result on the three
corpus models is not a broad parity claim.

Time to first result is already beyond parity because stanli avoids the model
C++ build. This proposal is about warmed gradient throughput.

## Current baseline and budget

The retained warmed-gradient means are:

| Model | stanli | CmdStan | CmdStan/stanli | Gap |
|---|---:|---:|---:|---:|
| `lotka_volterra` | 47.622 us | 41.313 us | 0.87x | 6.309 us |
| `soil_incubation` | 67.783 us | 60.871 us | 0.90x | 6.912 us |
| `one_comp_mm_elim_abs` | 522.915 us | 470.681 us | 0.90x | 52.234 us |

Using the callback counts retained with the earlier direct-seeding experiment,
mere parity requires removing approximately 24 ns, 21 ns, and 32 ns per
callback respectively. That is a small enough budget that allocations and
adapter copies must be measured before changing sensitivity algorithms.

Treat these rows as the target locator, not the Phase 0 baseline. The retained
CmdStan driver was generated without `stanc --O1`, and its compile recipe does
not reproduce all optimization flags used by a normal CmdStan model binary.
The one-compartment CmdStan cell also predates the shared SUNDIALS compile
recipe now used by the harness. Rebuild and remeasure every arm before using a
microsecond gap to admit an optimization.

The models exercise materially different paths: Lotka and soil use RK45 with
two states and roughly 250-330 callbacks per gradient; one-compartment uses
BDF/CVODES at tight tolerances, one state, a runtime branch, and roughly 1,650
callbacks. A result from one is not automatically a result for the others.

The current differentiated path is:

```text
OP_ODE forward
  -> Stan Math solver
    -> coupled sensitivity callback, many times
      -> construct nested var inputs
      -> run the RHS register program under var
      -> reverse once per RHS output to obtain J_y and J_theta
      -> advance states and sensitivities together
    -> construct precomputed-gradient solution nodes
  -> harvest their dense solution Jacobian into OP_ODE scratch

OP_ODE backward
  -> J_solution^T * output_adjoint
```

Both engines use the same Stan Math integrators and coupled sensitivity
equations. The available wins are at the stanli-specific callback boundary and
inside stanli's compiled RHS.

## Correctness repair before a performance claim

One audit result is a release blocker independent of performance:
`lower_ode_fn()` classifies only names containing `bdf` as stiff and maps every
other legacy `integrate_ode_*` name to RK45. Consequently
`integrate_ode_adams` currently runs RK45. The write-array interpreter repeats
that choice.

Make this a small PR before the optimization series:

- map each legacy name explicitly to `RK45`, `BDF`, or `ADAMS`;
- dispatch on `OdeSpec::solver` in both the graph and write-array paths;
- retain the exact legacy function name in Stan Math errors;
- test default and explicit tolerances with a system that distinguishes the
  methods; and
- add legacy RK45, BDF, and Adams to the CmdStan differential sweep.

The same parity track should then audit and expand:

- parameter-valued `t0` and output times, which the current lowerer requires to
  be constant and which have no OP_ODE Jacobian columns;
- matrix and nested-array variadic arguments, including preservation of their
  logical geometry in the interpreter fallback;
- modern ODEs in transformed data, transformed parameters, model locals, and
  generated quantities; and
- invalid time, tolerance, step-limit, RHS-size, nonfinite-RHS, and rejection
  behavior.

These items need not all block the narrow three-model experiment, but they do
block wording that stanli is broadly past CmdStan for ODEs.

## Phase 0: make the measurement decisive

First produce a clean baseline with the current source, pinned dependencies,
and explicit compiler recipes. Record the stanc optimization level and the
complete C++ flags for both engines. Include a normal CmdStan model-binary
configuration in addition to the small gradient driver so a harness artifact
cannot become the claimed competitor.

Add a same-binary diagnostic mode that records, per solve:

- callback count;
- attempted and accepted step count, wherever the solver exposes them;
- input/container marshalling time and allocation count;
- register seed, register-program body, and output extraction time;
- RHS derivative construction and reverse/tangent time;
- sensitivity propagation time;
- solver total;
- final solution-Jacobian harvest; and
- OP_ODE backward.

The diagnostic must compile out of release builds. Timers around a 20 ns region
are perturbative, so use them for attribution only; use uninstrumented
end-to-end A/B runs for decisions.

Mirror the work-count counters in the CmdStan driver. Current output values are
not bitwise identical to CmdStan, so equal tolerances do not prove equal step
histories. If at least half of a measured gap is different callback/step count,
first identify the value-ordering difference; a per-callback optimization is
not the primary explanation. Preserve `bench_grad`'s existing forward-only
field in corpus reports instead of discarding that free decomposition.

Benchmark both legacy and modern interfaces. The three corpus rows are legacy,
so a modern-only improvement can look successful while changing none of the
published losses.

## Phase 1: remove the callback boundary tax

### Change

Add a caller-owned output form beside `run_rhs`:

```cpp
run_rhs_into(program, t, y, theta, n_theta_source, x_r, output_ptr);
```

It must use the existing register seed order and `run_program` unchanged, then
copy output registers directly to caller-owned storage. Keep the vector-return
wrapper for existing callers and the MIR interpreter fallback.

Change `VarRhs` to allocate its required Eigen result and fill it directly.
Today it first creates a `std::vector<T>` in `MirRhs::eval`, then allocates an
Eigen vector and copies again.

For legacy calls, bypass Stan Math's
`integrate_ode_std_vector_interface_adapter`. Invoke the same internal
`ode_rk45_tol_impl`, `ode_bdf_tol_impl`, or `ode_adams_tol_impl` with `VarRhs`,
an Eigen initial state, the matching legacy function-name string, and the same
tolerances. Convert only the completed output collection. CmdStan's generated
legacy path still pays the public adapter's Eigen-to-vector-to-Eigen callback
round trip; stanli does not need to.

This phase deliberately changes no equation, derivative rule, sensitivity
layout, adaptive controller, tolerance, or solver implementation. Container
copies of `var` handles do not create different arithmetic nodes, but tests
must still pin the existing promotion order: `y`, every source theta including
the unread placeholder, `t`, then `x_r`.

### Gate

Ship the cleanup if it is neutral or faster everywhere and passes the complete
ODE oracle. Escalation depends on the end-to-end result:

- if all three models satisfy the final 1.05x gate, this already closes the
  narrow target; Phase 2 becomes headroom rather than a release blocker;
- if it recovers at least 3% but does not cross, retain it and proceed; and
- if it is noise, revert the production path while retaining the measurement
  evidence.

The hypothesis is credible, not proven: Phase 1 removes one stanli-owned output
temporary plus the legacy adapter conversions that CmdStan's public path still
pays, and the 21-32 ns budget is of the same order. Only paired
complete-gradient runs establish the result.

## Phase 2: compiled RHS derivative provider, unchanged solvers

### Interface

Give an eligible `RhsProgram` an optional derivative provider with one contract:

```text
evaluate(t, y, theta, x_r)
  -> f
  -> J_y       always when sensitivities are active
  -> J_theta   for active parameter lanes
```

`J_y` is required even when the initial state is data. Parameter sensitivity
evolves as `J_y * S_theta + J_theta`.

The provider has three tiers:

1. **Generated reverse.** For branch-free programs accepted by the existing
   adjoint rules, run the primal register program on doubles and seed the
   generated reverse once per RHS output. This is the first path for Lotka and
   soil.
2. **Generated tangent.** For a branch-bearing program with a small active
   width, execute a forward/tangent program in normal program order. It follows
   the same `JZ`/`JMP` path naturally. This is the path for one-compartment's
   `if (t > 0)`. A bounded `fvar<double>` proof may establish the ceiling, but
   production rules must match Stan Math's value and derivative grouping.
3. **Current var replay.** Unsupported opcodes, wide cases without a measured
   win, failed exactness checks, and interpreter-only RHS functions retain the
   existing behavior.

Reverse generation currently accepts only `IslandProg`, reads its live-ins,
stores its `AdjProgram`, mutates the forward code with checkpoints, and refuses
`kProgramNoAdjoint` instructions such as jumps. Refactor the reusable mechanism
around `Program + live-in spans + AdjProgram`; do not make ODE semantics depend
on pretending an RHS is a graph island.

An `RhsProgram` contains no graph `CALL` instructions, which removes the most
complex existing adjoint payload from this eligibility class.

Do not let adjoint generation mutate the canonical `RhsProgram`. Retain:

- the compact canonical primal used by values-only solves and the var-replay
  oracle; and
- a derivative-forward clone containing any inserted checkpoint `MOV`/`MOVR`
  instructions, its enlarged register file, and its `AdjProgram`.

Otherwise enabling compiled derivatives can tax every values-only callback and
silently change the fallback that is supposed to be the oracle.

An `RhsDerivativeProgram` should record:

- derivative kind (`reverse`, `tangent`, or none);
- value, adjoint/tangent, output, `y`, and theta register mappings;
- active-width limits;
- eligibility/fallback reason; and
- reusable thread-local value and derivative buffers.

### Stan Math bridge

Keep all four Stan Math solvers. When `MirRhs` is instantiated with `var` and a
compiled derivative provider is eligible:

1. evaluate the RHS value and requested Jacobian rows on doubles;
2. build one lightweight precomputed-gradient output node per RHS component,
   connected to the incoming state vars and active parameter vars; and
3. return those vars to Stan Math's existing coupled system.

Stan Math will still ask for one reverse sweep per RHS output, but each sweep
chains through one precomputed row instead of every arithmetic operation in the
RHS. The solver, state/sensitivity layout, accumulation order, coupled error
estimate, step controller, CVODES configuration, final output nodes, and
solution-Jacobian harvesting stay unchanged.

Use the low-level precomputed-gradient constructor with shared operand pointers
and stable Jacobian-row storage for the duration of a callback. Do not allocate
and copy a fresh operand and gradient vector for every output row. The nested
callback consumes all rows before the reusable storage can be overwritten.
This lifetime argument depends on Stan Math calling the RHS once and consuming
all output `.grad()` sweeps synchronously before the callback returns;
reentrant/nested ODE evaluation remains refused.

### B0 proof before integration

Before touching `MirRhs`, benchmark the derivative providers in isolation:

- value and full RHS Jacobian from current `run_rhs<var>`;
- double primal plus generated reverse for Lotka and soil; and
- double primal plus tangent columns for one-compartment.

Require exact RHS values, exact derivative rows where the corresponding Stan
Math rule is bitwise stable, and an end-to-end projected saving above 5%. A
microbenchmark with no complete-gradient projection is not an admission gate.

### Integrated gate

The fast and fallback paths must be selectable in one binary for A/B and oracle
testing. The integrated path must preserve:

- callback and accepted-step counts;
- solution values and dense scratch Jacobian at the existing reference points;
- final model log density and gradient;
- mixed activity for `y0` and theta;
- exception type/category and function name; and
- native and WebAssembly builds without executable memory.

If this bridge crosses the final performance gate, stop here. It provides the
main architectural advantage without owning an integrator.

### Adjudication against the all-double alternative

The independent Fable review preferred moving the coupled sensitivity system
out of nested autodiff immediately. That has the highest ceiling because it
removes work CmdStan also pays, rather than only stanli's differential. This
proposal deliberately inserts the precomputed-gradient bridge first:

- the measured deficit is only 21-32 ns per callback;
- the bridge tests the compiled derivative provider without changing solver
  invocation, error control, sensitivity layout, or CVODES configuration; and
- it gives a clean attribution point: any remaining cost after per-op `var`
  replay is removed belongs to the Stan Math protocol or the solver.

Escalate directly from the B0 derivative proof to Phase 3 only if a callback
ceiling measurement shows that constructing the bridge cannot clear the final
gate. Do not require an integrated bridge merely to confirm an already-failed
ceiling.

## Phase 3: direct coupled sensitivities, only with evidence

The precomputed-gradient bridge still constructs a nested autodiff scope and
solution-facing `var` nodes because that is Stan Math's callback protocol. If
Phase 2 attribution shows that protocol is now material, add a stanli-owned
coupled RHS for RK45 and CKRK first:

```text
f'       = compiled_rhs(t, y, theta)
S_y0'    = J_y * S_y0
S_theta' = J_y * S_theta + J_theta
```

The preferred RK45/CKRK prototype is not a copied solver loop. Present the
manually assembled coupled vector as an all-double ODE to the pinned Stan Math
prim entry point, then unpack values and sensitivities directly into OP_ODE
output and scratch. This reuses the same Boost Odeint tableau, dense-output
controller, tolerances, and max-step behavior while bypassing reverse-mode
callback construction. Verify that its initial coupled state, state layout,
observer behavior, loop order, and summation order match the rev path exactly
before enabling it. The adaptive controller must see the complete coupled
state. A states-only solve followed by separately integrated sensitivities is
numerically different and is not an option.

BDF and Adams come later. Pinned Stan Math's CVODES adapter separately owns the
primal RHS, state-Jacobian callback, and sensitivity RHS. Supplying a custom
coupled functor alone does not remove its generic autodiff Jacobian. A clean
implementation needs an upstream derivative-provider hook or a carefully
tested local CVODES adapter that passes compiled `J_y` and `J_theta` to both
callbacks. Do not fork that code before Phase 2 proves the remaining ceiling.

## Numerical and semantic contract

Adaptive solvers can amplify a last-bit Jacobian difference into a different
step history. Eligibility is therefore conservative.

For every derivative opcode or tangent rule:

- compare double-forward values with `run_program<var>().val()` over normal,
  boundary, signed-zero, infinite, and NaN cases admitted by Stan;
- compare derivative rows with current Stan reverse replay, using the exact
  Stan Math expression and operand grouping;
- preserve register checkpoint values and copy/alias adjoint semantics;
- preserve branch predicates and the executed path;
- reject unsupported dynamic indexing or range operations rather than
  approximating them; and
- keep one runtime fallback switch so the old path remains an oracle.

End-to-end tests compare both old and new paths before comparing either with
CmdStan. This separates a stanli optimization regression from an existing
CmdStan differential mismatch.

## Benchmark protocol and release gate

For each arm:

- use a release, native-architecture, sanitizer-free build with
  `-ffp-contract=off`;
- pin the same Stan Math and C++ toolchain where possible;
- warm for at least 200 ms;
- run at least ten paired rounds of at least 0.5 s per arm;
- alternate ABBA/BAAB order across fresh processes;
- test all three deterministic reference points, not only the published
  benchmark point; and
- report the paired estimate and 95% confidence interval.

Run these competitors separately:

1. stanli fallback;
2. stanli Phase 1 direct adapter;
3. stanli compiled derivative provider;
4. pinned default CmdStan; and
5. pinned CmdStan generated with `stanc --O1`.

The narrow release gate is:

- `CmdStan default / stanli >= 1.05` for each of the three ODE models;
- paired 95% CI lower bound above `1.00` for each;
- no corpus model regression above 3%; and
- confirmation on macOS arm64 and Linux x86_64 before a general performance
  claim.

Also sweep synthetic shapes over state count, active parameter count, output
time count, branch presence, and solver. A design that wins only for a two-state
RHS should be selected only for that eligibility class.

## Expected file sequence

**Correctness PR**

- `runtime/src/lower.cpp`: explicit legacy solver mapping.
- `runtime/kernels/ode.cpp`: dispatch legacy Adams correctly.
- `runtime/src/wa_interp.cpp`: match graph solver selection.
- `harnesses/ode_sweep.py` and ODE tests: discriminating legacy oracle.

**Phase 1 PR**

- `runtime/include/stanli/ode_prog.hpp`: `run_rhs_into`.
- `runtime/kernels/ode.cpp`: direct Eigen output and direct legacy `_tol_impl`
  calls.
- `tests/test_ode_prog.cpp` and `tests/test_odevariadic.cpp`: seed order,
  fallback, and interface parity.

**Phase 2a PR**

- `runtime/include/stanli/adjoint.hpp` and `runtime/src/adjoint.cpp`: reusable
  live-in-driven adjoint generation.
- `runtime/include/stanli/ode_prog.hpp` and `runtime/src/ode_prog.cpp`: RHS
  derivative payload and eligibility.
- `tests/test_adjoint.cpp` and `tests/test_ode_prog.cpp`: value/Jacobian oracle.
- a focused callback benchmark with complete-gradient projection.

**Phase 2b PR**

- `runtime/kernels/ode.cpp`: precomputed-gradient callback bridge and A/B
  switch.
- `tests/test_odevariadic.cpp`, `harnesses/ode_sweep.py`, and stored CmdStan
  references: end-to-end solver/value/gradient/error parity.

**Phase 3 PRs, only if admitted**

- a repository-owned coupled sensitivity adapter for RK45/CKRK;
- then an explicit CVODES derivative-provider seam for BDF/Adams.

## Stop rules

- Stop native/JIT work for tiny active RHS programs; the measured ceiling is
  insufficient.
- Stop at Phase 1 if it clears the final gate; do not add derivative machinery
  merely because it is interesting.
- Stop at the Phase 2 bridge if it clears the final gate; do not own solver
  code without measured need.
- If at least half of the baseline gap is different callback/step count, stop
  attributing that part to callback overhead and reconcile evaluation order
  first.
- Fall back on any unsupported instruction or exactness failure.
- Do not call the result broad ODE parity until the correctness track is
  complete.

## Strongest objection

The warmed gap is modest, the retained baseline has fairness/provenance
problems, and complete sampling time already favors stanli because CmdStan must
compile a model binary. Phases 2 and 3 add derivative and possibly
solver-adapter surfaces that must track pinned Stan Math. The answer is not to
assume that complexity is justified: Phase 0 refreshes the real bar, each phase
has a same-build oracle and a stop rule, and no solver-facing code is admitted
unless the preceding ceiling measurement shows that the simpler path cannot
clear the release gate.
