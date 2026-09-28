# Interpreter and execution-path audit — 2026-09-28

> **Output-path update:** runtime real callback arguments now stay compiled in
> generated quantities with double-only solver execution and no derivative
> scratch. [Results, costs and remaining fallbacks](2026-09-28-callback-runtime-values-results.md).
> The next general-runtime [design decision](2026-09-28-general-value-program-decision.md)
> concerns dynamic values/call frames; it proposes no JIT work.

> **Callback geometry update:** plain matrix arguments now retain their shapes
> through ODE, DAE, quadrature, variadic algebra and adjoint callbacks, including
> genuine interpreted fallbacks. [Validation, performance and remaining gaps](2026-09-28-callback-geometry-results.md).

> **Transform update:** ordinary Cholesky-correlation reverse now uses retained
> scalar intermediates; exceptional histories keep the Stan tape. Measured
> Kronecker GP inference improves 15.8%; [results and storage costs](2026-09-28-cholesky-correlation-results.md).


> **Native performance update:** normal_id_glm now records Stan's analytical
> partials without a nested tape. Small Gaussian gradients improve 27.6% in the
> measured native case; [results and limitations](2026-09-28-normal-glm-results.md).


> **Progress update:** Shared register compilation now handles fixed-shape early
> returns through branches and loops; see [implementation, validation and native
> measurements](2026-09-28-function-exits.md). Eligible standalone functions now
> use cached register plans; see [results and remaining refusals](2026-09-28-standalone-function-results.md).
> Earlier inventory statements below describe the audit baseline, not this
> completed return coverage. Native instruction generation remains tabled.

> **Coverage update:** Subsequent scalar and vector RNG slices close all 13
> originally missing RNG names and the categorical register-context gaps for
> the admitted scalar/single-vector forms. This is name coverage, not complete
> overload or shape coverage. See the [current implementation record](2026-09-28-vector-rng-implementation.md).
> Native instruction generation, stencil JIT and dispatch-JIT are tabled by
> user direction; continue with existing-engine coverage and performance.

Audited source: `0bb5b54c8dfa7e9ee40d109209495fe7a9024508` (0.17.1), fetched
`origin/HEAD` → `origin/main`. The clean starting worktree was three commits
behind. Created `codex/interpreter-gap-audit` from that fetched ref and verified
`git merge-base --is-ancestor origin/HEAD HEAD`. No integration conflicts.

This was a source audit, including existing tests and retained reports, not a
fresh execution or performance census. At the time of the audit there was no
build directory, and no runtime changes, model runs, or benchmarks were made.
The subsequent [census implementation](2026-09-28-execution-census-implementation.md)
records new runtime work and validation. Performance priorities below remain
hypotheses until measured on current artifacts.

The follow-up [execution-engine explanation and roadmap](2026-09-28-execution-engines-and-roadmap.md)
separates dispatch, kernel differentiation, and retained reverse state, and
defines staged implementation and deletion gates. In particular, a generated
region adjoint can still call a kernel that uses a nested tape internally.
The [Fable review and dispositions](2026-09-28-fable-roadmap-review.md) prompted
the clarifications below. Review used `5b913866`, whose runtime sources match
this audit's original baseline; it did not run models or benchmarks.

**Three different kinds of work remain.**

1. Replace calls to `MirInterp<T>`, the MIR tree interpreter.
2. Improve already compiled execution: register dispatch, nested Stan Math
   autodiff tapes, repeated factorizations, and preparation/storage costs.
3. Implement currently unsupported language/functions. These do not become
   supported merely by enabling the interpreter.

Deleting `MirInterp` is feasible in principle without introducing a native
JIT. It requires a compiled replacement for every currently supported use of
the interpreter, including preparation and initialization. Eliminating all
instruction interpretation would be a separate machine-code/WebAssembly
generation project: `run_program` and the graph/structured executors are also
dispatch engines.

**Where the MIR interpreter is still used.**

| Entry point | Current role | Replacement needed |
| --- | --- | --- |
| Transformed data and compile-time evaluation | `Lowering::td` is a `MirInterp<double>`; evaluates `prepare_data`, conditions, size expressions, and data-only folds. Recognized input hydration has a preload shortcut. | A value-only compiled program with data access, dynamic storage where needed, shape queries, validation, effects, and the construction RNG stream. |
| Lowering-time folding and admission probes | `try_eval_interpreter` evaluates selectors and short-circuit conditions against partial environments; structured-loop data probes construct a fresh interpreter with a 4096-statement budget. These overlap the data evaluator above but are separate deletion obligations. | A compiled value evaluator callable during lowering, with partial-environment access, bounded work, preserved effects/short-circuiting, transactional state and equivalent refusal behavior. |
| Constrained initialization | `InitInterp` runs `transform_inits` when the caller supplies constrained values. Bounds may depend on an earlier parameter. | Compiled sequential reads/inverse transforms, preserving parameter dependencies, declaration order, validation, and serialization. |
| Standalone Stan function API | `stanli_function_call` constructs a fresh `MirInterp<double>` for every invocation. This is the normal path, not an exceptional fallback. | Compile/cache callable programs against logical argument types and shapes; preserve runtime values and handle shape-dependent specialization explicitly. |
| Generated quantities / write-array | If lowering truncates, `WaInterp` executes the whole generated-quantities section, including transformed parameters. Drivers prefer it over the partial graph. | Complete RNG/control/shape lowering, or introduce a carefully specified mixed execution boundary. A whole-section fallback currently amplifies the cost of one missing operation. |
| Retained callbacks | ODE RHS, adjoint ODE RHS, DAE residuals, algebraic systems, and quadrature integrands try `RhsProgram`; `!prog.ok` invokes `MirInterp<T>`. | Close shared `ProgramCompiler` gaps and callback argument-layout gaps. One shared fix can benefit multiple solver families. |

Sources: [lowering state](../runtime/src/lower_internal.hpp),
[model construction](../runtime/src/lower.cpp),
[initialization](../runtime/src/init_interp.cpp),
[folding probes](../runtime/src/lower_expr.cpp),
[bounded admission probes](../runtime/src/lower_structured_loop.inc),
[standalone functions](../runtime/src/function.cpp),
[write-array](../runtime/src/wa_interp.cpp), and the callback dispatches in
[ODE](../runtime/kernels/ode.cpp),
[adjoint ODE](../runtime/kernels/ode_adjoint.cpp),
[DAE](../runtime/kernels/dae.cpp),
[algebra](../runtime/kernels/algebra.cpp), and
[quadrature](../runtime/kernels/quadrature.cpp).

The solver fallback adapters construct a fresh `MirInterp<T>` per callback
evaluation. The source establishes that construction pattern, not its current
timing cost. Transformed-data hooks can also execute retained higher-order
calls, so the preparation census must include those nested mechanisms.

`STANLI_NO_INTERPRETER=1` is not a proof that no interpreter ran. It checks
`CompiledModel::interpreter_fallbacks` at the end of model compilation; it does
not prohibit the preparation interpreter, initialization, or the standalone
function API. There is also a source-level reporting gap: legacy
`integrate_ode_*` lowering compiles its RHS without adding a fallback note when
that compilation fails. `emit_ode` only emits a debug diagnostic, and the
legacy branch in `lower_program_ode` likewise lacks the note present in its
modern branch. This should get a focused regression test before making a
strict no-interpreter claim. It was identified statically, not reproduced by
execution in this audit.

**Concrete generated-quantities gaps.**

The explicit name dispatch in the current source has 29 interpreted RNG names
and graph routes for 16 of those names. These are name counts, not overload
coverage. The 13 names implemented by `interpreted_rng_call` without a graph
route are:

| Scalar-distribution RNGs | Other RNGs |
| --- | --- |
| `std_normal_rng`, `gamma_rng`, `inv_gamma_rng`, `beta_rng`, `chi_square_rng`, `cauchy_rng`, `double_exponential_rng`, `logistic_rng`, `weibull_rng`, `neg_binomial_2_rng`, `neg_binomial_2_log_rng` | `multi_normal_cholesky_rng`, `poisson_binomial_rng` |

The eleven scalar-distribution names are a bounded first coverage patch: share
their existing Stan Math calls through the RNG family registry, then make
graph and register-program lowering use that same contract. Preserve exact
stream advancement and validation; substituting an algebraically equivalent
distribution formula is insufficient.

Coverage also differs by context:

- Graph scalar-RNG lowering accepts scalar and vector/row-vector arguments,
  but refuses Stan array arguments. Vector draws are expanded into individual
  `OP_RNG` operations and concatenations, leaving a batching opportunity.
- Register-region scalar RNGs require scalar arguments/results.
- Ordinary graph lowering supports `categorical_rng` and
  `categorical_logit_rng`; the region RNG handler explicitly refuses
  `categorical_rng`, and does not list `categorical_logit_rng`.
- Both graph and region handlers have single-vector `dirichlet_rng` and
  covariance-form `multi_normal_rng` paths; “all vector RNGs are interpreted”
  is no longer accurate.
- Runtime-sized results/locals, dynamic loop bounds and selectors beyond the
  admitted fixed-shape operations still leave gaps. Runtime integer division,
  unproved integer reductions, and some extrema expression forms also have
  explicit graph-side refusals. Context determines whether another compiled
  route handles the expression or write-array falls back.

Sources: [RNG registry](../runtime/src/function_registry.cpp),
[graph handlers](../runtime/src/lower_funapp.cpp),
[region compiler](../runtime/include/stanli/mir_prog.hpp), and
[interpreter RNG handlers](../runtime/src/wa_interp.cpp).

**Callback and control-flow gaps.**

`ProgramCompiler` already handles arithmetic, shared kernel `CALL`s, many
container operations, runtime `if`/ternary branches, and runtime `while` loops.
The backlog is more specific:

- Arbitrary early returns beneath runtime control are refused. A recognized
  pair of terminal returns with matching logical views already works.
  `test_ode_prog.cpp` explicitly expects `f_early` to require interpretation.
- `for` bounds still go through compile-time integer evaluation in the flat
  register compiler. Shape-changing locals/conditional arms and general
  dynamic slices need a richer storage contract.
- Dynamic scalar indexing exists, but arbitrary multidimensional runtime
  selectors and integer-array mutation in a structured `while` remain limited.
- The callback entry's `supported_rhs_view` admits real/int scalars, vectors,
  row vectors, and one-dimensional scalar arrays. Matrices, arrays of vectors,
  and higher ranks fail that entry check. Falling back does not by itself prove
  that the interpreter's positional argument adapter preserves such shapes.
- The register cap is `1 << 20`; large unrolled bodies can exhaust it. The
  `interp_fallback_ode.stan` fixture intentionally exceeds it. Raising the cap
  alone would not solve preparation and memory growth.
- UDFs are inlined with a depth guard rather than executed through general
  runtime call frames. Runtime recursion/calls require a separate design.

In ordinary log-prob control flow, a refused region compiler is a compilation
error, not an automatic whole-model MIR interpreter fallback. The fallback
policy is specific to the caller.

Sources: [callback compilation](../runtime/src/ode_prog.cpp),
[program compiler](../runtime/include/stanli/mir_prog.hpp),
[region lowering](../runtime/src/lower_stmt.cpp),
[ODE tests](../tests/test_ode_prog.cpp), and
[register-limit fixture](../tests/fixtures/interp_fallback_ode.stan).

**Compiled paths that still have avoidable work.**

| Nodes / functions | Current remaining cost | Candidate improvement |
| --- | --- | --- |
| `OP_ISLAND` / register control regions | Acyclic branches can have generated adjoints. Back edges, dynamic index/set instructions, transforms, print, several matrix instructions, vector-density instructions, and active extrema can make `gen_adjoint` refuse, leaving nested-var replay. | Extend derivative coverage with explicit value lifetimes and executed-path history; use existing kernel `CALL` derivatives where appropriate. General loops require iteration history, not just one flag per basic block. |
| `OP_ODE` | Direct generated RK sensitivities require RK45/CKRK, eligible shapes, fixed times in the two-input form, and an exact-forward opcode whitelist. Branches, `DOT`, `CALL`, and many other valid compiled instructions miss this fast path. | Extend exact derivative eligibility incrementally; preserve adaptive solver step history. BDF/Adams and active-time support are separate work. Region-created ODE payloads also do not go through `emit_ode`'s direct-RK setup. |
| `OP_DAE`, `OP_ODE_ADJOINT`, `OP_ALGEBRA_SOLVER`, `OP_QUADRATURE` | A compiled callback does not remove Stan Math's differentiation of the solver/integral. These wrappers still use var/Jacobian machinery where derivatives are needed. | Profile callback evaluation versus solver differentiation before replacing anything; retain upstream algorithms. |
| Generic scalar binary graph kernels | 26 real/real or mixed integer/real function names use nested tapes in backward. | Profile common names first, then port exact Stan pullbacks or reuse partial-propagator interfaces. |
| `trigamma`, `student_t_qf`, `poisson_binomial_*`, `prod` | Algorithmic/ordered reverse work still uses Stan Math tapes. | Preserve algorithmic accumulation and edge cases; do not replace these with an unvalidated closed-form derivative. |
| Graph `softmax`, `log_softmax` | Nested varmat replay in backward, including per-leaf work for arrays. Register `SOFTMAX` already has a generated derivative. | Reuse a common proven kernel while preserving source reduction order and views. |
| `inverse`, `inverse_spd`, `log_determinant`, `crossprod` | Generic matrix backward replay remains. Active `inverse_spd` also constructs a tape in forward to match the selected Stan overload. | Retain forward results/factors and use the upstream reverse algorithm directly when numerically equivalent. |
| Matrix solve family | Active right plain/triangular forwards still use var arithmetic. Plain-left solves retain QR; right/SPD reverse paths still recompute factors. | Extend factor retention only where the exact forward/backward factor and triangle agree. |
| Structured parameter constraints | Unit vector, sum-to-zero vector/matrix, correlation/covariance and their Cholesky transforms, and stochastic matrix transforms retain taped backwards. Simplex/ordered/positive-ordered already have direct backwards. | Reuse upstream transform pullbacks with the Jacobian contribution and batch order intact. |
| `OP_LOOP` | Structured recording/replay reduces repeated work, but still retains values/control history. Admission refuses embedded register fallback and RNG/print/reject stateful calls; unknown geometry and early returns remain boundaries. | Expand proof coverage and reduce retained state/recording work after phase and memory attribution. This is distinct from MIR interpretation. |
| `reduce_sum`, `map_rect` | `reduce_sum` defaults to one thread and retains serial lowering on worker/admission refusal. `map_rect` expands serial job calls. | Evaluate parallel execution on sufficiently large workloads; serial execution is often appropriate for small models. |

The 26 binary names are: `atan2`, `beta`, `fdim`, `fmax`, `fmin`, `fmod`,
`gamma_p`, `gamma_q`, `hypot`, `lbeta`, `lchoose`, `lmultiply`,
`log_falling_factorial`, `log_inv_logit_diff`,
`log_modified_bessel_first_kind`, `log_rising_factorial`, `owens_t`,
`bessel_first_kind`, `bessel_second_kind`, `modified_bessel_first_kind`,
`modified_bessel_second_kind`, `binary_log_loss`, `lmgamma`,
`falling_factorial`, `rising_factorial`, and `ldexp`. This describes their
generic graph kernels; specialized instructions such as register `FMAX`/`FMIN`
can already have faster derivatives.

Probability kernels also retain a substantial tail: Wishart/inverse-Wishart
and Cholesky variants; `multi_gp` and `multi_student_t` families;
`multinomial_logit` and `dirichlet_multinomial`; LKJ densities;
`normal_id_glm` and `binomial_logit_glm`; the three `von_mises` distribution
functions and `neg_binomial_2_lcdf`/`lccdf`. Some compute and cache partials
once; others replay for backward. They should not be treated as one cost class.

Several important families have **shape-dependent** fast paths already:

- `gp_exp_quad_cov`: fixed locations, no fused jitter input, ordinary finite
  arithmetic; other covariance variants/active locations/exceptional numerics
  retain replay.
- `multi_normal_lpdf` / `multi_normal_cholesky_lpdf`: specific single-vector
  forms have direct paths; more general vectorized forms retain tape work.
- `dirichlet_lpdf`: shared vectors use the recorder; arrays retain replay.
- `ordered_logistic_lpmf` / `ordered_probit_lpmf`: eligible shared-cutpoint
  layouts are accelerated; other layouts retain replay.
- `wiener_lpdf`: a scalar fixed observation caches partials from one tape;
  weighted/nonfinite/general backwards retain replay.

Recent work already accelerated ordinary `multinomial_lpmf`, categorical and
ordered-logistic GLMs, Cholesky decomposition, symmetric eigendecomposition,
and parts of the families above. The filename `legacy_fns.cpp` or an old
comment in `matrix_fns.cpp` is not an authoritative capability classification.

Sources: [adjoint admission](../runtime/src/adjoint.cpp),
[program instruction flags](../runtime/include/stanli/program.hpp),
[scalar binary kernels](../runtime/kernels/scalar_binary.cpp),
[opcode/function lists](../runtime/include/stanli/optable.hpp),
[matrix kernels](../runtime/kernels/matrix_fns.cpp),
[solve kernels](../runtime/kernels/matrix_solve.cpp),
[constraints](../runtime/kernels/constrain.cpp),
[structured lowering](../runtime/src/lower_structured_loop.inc), and
[higher-order lowering](../runtime/src/lower_higher_order.cpp).

**Unsupported surface is a separate backlog.**

Current source and the checked-in conformance classifications identify missing
`hypergeometric_1F0`, `hypergeometric_2F1`, `inc_beta`, `inv_inc_beta`,
`wiener_lcdf_unnorm`, and `wiener_lccdf_unnorm` graph functions; extended
`wiener_lpdf` overloads beyond the five-input form; `gp_periodic_cov`; and
`gaussian_dlm_obs_lpdf`. The latter has an explicit seven-arguments-versus-six
graph-input refusal. That descriptor limit is an implementation choice; packed
operands already solve analogous live-in limits for other nodes. Complex
values, tuple results, and general recursive model lowering also need broader
language representation work. Standalone function calls already support
bounded recursion: `tests/test_function.cpp` checks success, depth refusal and
recovery. That API's recursive behavior must survive interpreter deletion.
The recorded unsupported classifications are historical
evidence, not a fresh support run on this SHA.

**Recommended order and deletion gates.**

1. Make execution paths observable per phase and call site: tree interpreter,
   register/generated reverse, register/var replay, kernel tape, or direct
   kernel; include shape/activity and refusal reason. Start with final-selection
   manifests, preparation-entry evidence and existing inclusive profiles;
   defer detailed dynamic counts, exclusive timing and memory attribution to
   focused follow-ups. Parent and child inclusive times overlap.
   Fix the legacy ODE reporting omission and distinguish a hot-path policy
   from a strict prohibition on every interpreter entry point. Existing
   `wa_coverage.py` and `op_census.py` are useful starting points, but neither
   alone supplies this inventory.
2. Close the 13 RNG name gaps and the graph/region RNG inconsistencies using
   shared handlers. Follow with array argument support. Validate the complete
   output and subsequent RNG engine state, including repeated calls and
   refusal/error cases.
3. Compile standalone function calls and close shared callback early-return,
   shape, indexing and register-growth gaps. Normalize UDF exits into explicit
   control flow; avoid accumulating further syntax-specific exceptions.
4. Prioritize nested-tape removal from measured profiles. Cheap scalar
   binaries, transform backwards, matrix replay and common density layouts
   are candidates; mathematical simplicity alone is not evidence of numerical
   equivalence or a useful performance gain.
5. Move transformed data and constrained initialization onto the compiled
   value engine, including runtime allocation, validation, inverse transforms,
   effects, seeded RNG semantics, and lowering-time folding/admission probes
   over partial environments with bounded refusal. These cold paths can be later performance
   priorities but are mandatory for deleting `MirInterp`.
6. Keep the old engine as a test oracle during migration, then require a build
   with its production entry points disabled to pass the supported-surface
   suites. Remove it only after that build works in native and browser modes.

For every migration, require positive coverage and adversarial refusal cases,
plus CmdStan as the independent oracle. Compare log density, every gradient,
output names/shapes/order, rejection behavior, and non-finite classifications.
Retain existing tighter checks and investigate discrepancies against the
project's 10-ULP goal rather than loosening gates. For RNGs, preserve exact
draw ordering and subsequent engine state. For control/effects, preserve
evaluation count and phase. Measure preparation, first/warm gradients,
per-draw work, complete inference, peak/retained memory, and package size as
appropriate; include ordinary small/medium models alongside stress cases.

The historical 119/119 graph-backed write-array result in
[OPTIMIZATIONS.md](../runtime/src/OPTIMIZATIONS.md) demonstrates progress on
that corpus, not complete RNG/language coverage. That document's older claims
about parameter-dependent sampling and while loops also lag current code.
Retained corpus benchmark binaries identify base `2c9d67b9`, not this audit's
HEAD. No current speedup or fallback-frequency claim follows from those
artifacts without a new matched run.
