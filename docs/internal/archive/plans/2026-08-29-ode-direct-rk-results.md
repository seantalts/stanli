# Direct compiled RK sensitivities: production result

**Status:** the RK45/CKRK implementation is complete and default-on for
eligible compiled RHS programs. It is rebased onto `origin/main` at
`d40e8a35b50ce8bc7ece3c402da0060f944a7844`. macOS arm64 admission evidence
is complete; Linux x86-64 Release CI remains the merge gate.

## Outcome

The bounded ceiling prototype translated cleanly into the runtime and retained
nearly all of its measured gain. For an eligible RK call, stanli now integrates
the primal state and every active sensitivity together in double precision.
Each RHS callback runs one compiled double forward and generated register
reverse sweeps to obtain exact `f`, `J_y`, and `J_theta`; it creates no nested
Stan Math autodiff graph.

The current Stan Math implementation remains the exact oracle and fallback.
Selection is fail-closed at lowering: unsupported instructions, control flow,
an unavailable compiled RHS, non-RK solvers, or a shape mismatch retain the
old path. `STANLI_NO_ODE_DIRECT_RK=1` selects the oracle when a model is
lowered, which supports fresh-process differential and performance tests
without a branch in the callback.

This is not a revival of the stopped Phase 0 callback bridge. That prototype
constructed precomputed-gradient Stan Math nodes and failed its mandatory
callback-level speed gate. The admitted path instead owns the complete coupled
RK solve in double space and uses generated reverse only as a local Jacobian
provider.

## Retained end-to-end performance

The retained run used 200 ms warmup per arm, calibrated every timed arm to at
least 0.5 seconds, then ran 15 alternating ABBA/BAAB fresh-process paired
batches. Estimates are geometric means of paired ratios; intervals are
two-sided Student-t 95% intervals in log-ratio space.

| model | comparison | speedup | paired 95% CI |
|---|---|---:|---:|
| Lotka--Volterra | stanli oracle / direct RK | **1.968x** | [1.951x, 1.985x] |
| Lotka--Volterra | CmdStan default / direct RK | **2.157x** | [2.094x, 2.221x] |
| Lotka--Volterra | CmdStan `--O1` / direct RK | **2.201x** | [2.184x, 2.218x] |
| soil incubation | stanli oracle / direct RK | **1.958x** | [1.944x, 1.971x] |
| soil incubation | CmdStan default / direct RK | **2.266x** | [2.241x, 2.291x] |
| soil incubation | CmdStan `--O1` / direct RK | **2.207x** | [2.179x, 2.235x] |
| one-compartment BDF | stanli oracle / current | 0.997x | [0.988x, 1.006x] |
| one-compartment BDF | CmdStan default / current | **1.068x** | [1.061x, 1.075x] |
| one-compartment BDF | CmdStan `--O1` / current | **1.082x** | [1.068x, 1.097x] |

The BDF row is deliberately neutral for the stanli A/B comparison: the new
selector does not admit BDF, so both arms execute the same current runtime.
It confirms that the inverse selector itself does not perturb ineligible
models. Relative to CmdStan, the earlier Phase 1 direct-output work remains
visible.

The earlier synthetic ceiling screen covered state count, active-parameter
width, output-time count, solver, activity masks, and branch presence. Every
admitted branchless case won: 18/18 RK45 cases had median `1.588x` speedup
(range `1.227x`--`2.615x`) and 18/18 CKRK cases had median `1.545x`
(range `1.160x`--`2.035x`). Branched programs retained the oracle.

## Exactness and behavior evidence

- A local hand-built RHS containing every admitted opcode matches the
  canonical var program bitwise for values and the complete `J_y/J_theta`.
- Opcode admission defaults to refusal. `DOT`, reductions, calls, jumps, and
  `FMAX/FMIN` are refused; the last pair is excluded because signed-zero ties
  do not reproduce Stan Math var semantics exactly.
- All RK45 and CKRK activity masks, legacy RK45, direct and oracle full
  Jacobians, scratch layout, and pullbacks match in the focused runtime test.
- Separately lowered default-on and `STANLI_NO_ODE_DIRECT_RK=1` models match
  exactly for complete output, gradient, stderr, and return behavior.
- Invalid tolerance, empty/unordered/nonfinite time and data inputs, maximum
  step exhaustion, and solver failure retain the exact exception type and
  message.
- Four threads sharing one immutable `OdeSpec` completed six repeated exact
  comparisons each, exercising the thread-local reusable workspace.
- The cross-path corpus reports one live direct-RK fixture and verifies that
  the inverse selector returns it to the oracle.
- Three real models at three parameter points matched exactly between stanli
  direct and oracle paths (9/9). A live CmdStan differential covered 16
  interfaces with a worst absolute difference of `2.40e-14`, below `1e-9`.
- The production synthetic manifest covered 84 configurations at three
  parameter points (252 exact default/oracle comparisons).

## Implementation boundary

The direct path deliberately mirrors the pinned Stan Math integration
contract: Boost RK45 or Cash--Karp tableau, initial step, controller, complete
coupled-state error norm, output schedule, step checker, validation order,
exception translation, and row-major harvested Jacobian. State, sensitivity,
register, value, and local-Jacobian buffers are reused for the solve through a
thread-local workspace. The graph-level `OP_ODE` backward pass is unchanged.

Normal Release builds contain no attribution timers or benchmark counters.
The ceiling and production benchmark targets are `EXCLUDE_FROM_ALL`; the
production runner checkpoints results atomically after every comparison and
records hashes for the benchmark binary, MIR, data, and CmdStan executables.

## Provenance and retained artifacts

- Source base for the retained timing run:
  `4f697ae40d55b8647d290d72774beecf174d859e`.
- Final rebase and correctness base:
  `d40e8a35b50ce8bc7ece3c402da0060f944a7844`.
- Host: Apple M3 Ultra, macOS 26.4 arm64; Apple clang 21.0.0; Release
  `-O3 -DNDEBUG -ffp-contract=off`.
- CmdStan: `11cb052d3e1fc8c799e0fec559e2ee5452b38d27`.
- Retained production JSON:
  `/private/tmp/stanli_ode_production_isolated_20260829.json`.
- Benchmark binary SHA-256:
  `81e9938c34b80fddc0878c94932542b0d044b19eb62f66d3189a82a9b3d1f2f2`.
- Synthetic manifest:
  `/private/tmp/stanli_ode_synthetic_v6_0c89544/manifest.json`.

Older files named `stanli_ode_production_4f697ae.json` and
`stanli_ode_production_final_4f697ae.json` were overlapping or interrupted
diagnostics and are not evidence for this decision.

## Recommendation

Merge the direct RK45/CKRK path after Linux x86-64 Release CI repeats the
correctness suite and paired regression gate. Continue BDF/Adams work in a
separate change: the branch-aware CVODES provider seam remains promising, but
the measured fvar prototype was not bitwise exact and must not be promoted.
Do not revive the Phase 0 precomputed-gradient callback bridge or use one
forward-tangent replay per active input as the general derivative provider.
