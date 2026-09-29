# Document catalog

Lookup inventory for repository-owned Markdown documents (including published
reports and source-adjacent guides). Vendored dependencies and generated MIR are
excluded. Start with [the short index](README.md); do not read this entire catalog
or its linked archives into context by default. Status labels identify document
roles, not a claim that every proposal was implemented.

Raw non-Markdown evidence is indexed separately in [artifacts](artifacts/README.md).
Execution result JSON lives beside its reports under `notes/execution/data/`;
older performance evidence is under `notes/performance/`. Keep embedded source
paths and hashes in raw records unchanged.

## Entry points and human guides

| Document | Contents / use |
| --- | --- |
| [Project priorities](../../AGENTS.md) | Repository policy and required research reading. |
| [Changelog](../../CHANGELOG.md) | Release-by-release behavior, compatibility and packaging changes. |
| [Project overview](../../README.md) | Project overview, package entry points, architecture and measured headlines. |
| [Why should I trust this?](../../TESTING.md) | Numerical gates, exceptions, validation and CI. |
| [Third-party components in the stanli binary](../../THIRD_PARTY_LICENSES.md) | Bundled dependency licenses and attribution. |
| [Documentation](../README.md) | Human reading map by task: use, understand, validate or contribute. |
| [2026-09-11 · Performance measurements](../benchmark-2026-09-11.md) | Human-facing guide or maintained technical contract. |
| [Historical benchmark experiments](../benchmark-history.md) | Human-facing guide or maintained technical contract. |
| [Corpus benchmark protocol, version 4](../benchmark-protocol.md) | Timed boundaries, numerical gate, repetition and publication rules. |
| [Benchmarks](../benchmarks.md) | Published model comparisons; accepted pairs, incomplete cases and limits. |
| [brms performance and numerics](../brms-performance.md) | Human-facing guide or maintained technical contract. |
| [Build and pull-request latency](../build-performance.md) | Human-facing guide or maintained technical contract. |
| [Compact densities: exact gradients, lp\_\_ off by a constant](../compact-densities.md) | Human-facing guide or maintained technical contract. |
| [Corpus status](../corpus-status.md) | Recorded model support and reference coverage, distinct from fresh replay. |
| [Probability-function coverage](../coverage.md) | Builtin signature coverage and unsupported overloads. |
| [Coming from cmdstanr](../from-cmdstanr.md) | Human-facing guide or maintained technical contract. |
| [Hacking on stanli](../hacking.md) | Code ownership, development recipes, diagnostics and validation. |
| [How stanli works and why it is fast](../how-it-works.md) | Architecture and execution explained for readers new to the runtime. |
| [Research index for agents](README.md) | Start here before research; choose one topic. |
| [Historical plans and reviews](archive/README.md) | How to interpret archived plans and recover full versions. |
| [Machine-readable evidence](artifacts/README.md) | Raw evidence owners, readers and provenance rules. |
| [Compiler, layout and preparation research](research/compiler.md) | Short topic map; follow links only as needed. |
| [Execution coverage and interpreter research](research/execution.md) | Short topic map; follow links only as needed. |
| [Loops, recording and memory](research/loops.md) | Short topic map; follow links only as needed. |
| [Numerical fidelity and measurement evidence](research/numerics.md) | Short topic map; follow links only as needed. |
| [Packaging, interfaces and installation](research/packaging.md) | Short topic map; follow links only as needed. |
| [Native parallel reductions](research/parallelism.md) | Short topic map; follow links only as needed. |
| [The lite build: half the library, lp__ off by a constant](../lite-lp.md) | Human-facing guide or maintained technical contract. |
| [Tutorial: three small models through every layer](../lowering-walkthrough.md) | Three small models traced through compiler and execution layers. |
| [Native within-chain reductions](../native-reduce-sum.md) | Supported opt-in within-chain parallelism and API settings. |
| [Native stanfit compatibility](../stanfit-compatibility.md) | Human-facing guide or maintained technical contract. |
| [Generated models and R workflows](../teaching-support.md) | Human-facing guide or maintained technical contract. |
| [Teaching Bayesian workflow with stanli](../teaching.md) | Human-facing guide or maintained technical contract. |
| [JavaScript and browser guide](../../js/README.md) | Browser/Node package, compilation, sampling and deployment. |
| [Research evidence](../../notes/README.md) | Where to write and find topical research evidence. |
| [Python interface guide](../../python/README.md) | Python installation, model/function APIs, sampling and outputs. |
| [stanli for R](../../r/README.md) | R installation, fitting, draws, diagnostics and ecosystem interfaces. |

## Corpus and test guides

| Document | Contents / use |
| --- | --- |
| [Generated Stan conformance harness](../../harnesses/conformance/README.md) | Fixture scope, provenance or test workflow. |
| [Models from brms](../../tests/brms/README.md) | Fixture scope, provenance or test workflow. |
| [Compiler producer fixtures](../../tests/compiler/README.md) | Fixture scope, provenance or test workflow. |
| [Stan teaching-model corpus for Stanli](../../tests/educational/IMPORT_README.md) | Fixture scope, provenance or test workflow. |
| [Models from Aalto Stan lessons](../../tests/educational/README.md) | Fixture scope, provenance or test workflow. |
| [Historical Aalto fixture experiment — 2026-09-14](../../tests/educational/RESULTS.md) | Fixture scope, provenance or test workflow. |
| [Integrated function coverage](../../tests/function_coverage/README.md) | Fixture scope, provenance or test workflow. |
| [Stan source lit tests](../../tests/lit/README.md) | Fixture scope, provenance or test workflow. |
| [Provenance and licensing](../../tests/rethinking/PROVENANCE.md) | Fixture scope, provenance or test workflow. |
| [Models from rethinking](../../tests/rethinking/README.md) | Fixture scope, provenance or test workflow. |
| [Language models, from stanc3's own test suite](../../tests/stanc3/README.md) | Fixture scope, provenance or test workflow. |

## Execution research

| Document | Contents / use |
| --- | --- |
| [2026-09-29 · Bounded vector output blocks](../../notes/execution/2026-09-29-bounded-vector-blocks.md) | Vector/row-vector admission, shape and sum proofs, MIR indexed-write correction, and native comparisons. |
| [2026-09-29 · Bounded generated-quantity blocks](../../notes/execution/2026-09-29-bounded-output-blocks.md) | Closed-block admission, resource limits, logical-length tests, existing structured execution policy, and native measurements. |
| [2026-09-29 · Execution inventory and bounded-storage probe](../../notes/execution/2026-09-29-execution-inventory-and-bounded-storage.md) | Corpus and fixture execution selections, scoped runtime traces, logical-length correctness fix, native measurements and remaining admission design gates. |
| [2026-09-28 · Next coverage slice: shape-preserving retained callbacks](../../notes/execution/2026-09-28-callback-geometry-plan.md) | Proof/evaluator or decision; check status before proposing work. |
| [2026-09-28 · Matrix geometry at retained callback boundaries](../../notes/execution/2026-09-28-callback-geometry-results.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Callback return safety before broader exit lowering](../../notes/execution/2026-09-28-callback-return-guards.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Compile value-only callbacks with runtime real arguments](../../notes/execution/2026-09-28-callback-runtime-values-plan.md) | Proof/evaluator or decision; check status before proposing work. |
| [2026-09-28 · Compile value-only runtime callback arguments](../../notes/execution/2026-09-28-callback-runtime-values-results.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Cholesky correlation: exact retained scalar reverse](../../notes/execution/2026-09-28-cholesky-correlation-plan.md) | Proof/evaluator or decision; check status before proposing work. |
| [2026-09-28 · Retained Cholesky-correlation reverse](../../notes/execution/2026-09-28-cholesky-correlation-results.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Decision: the next execution architecture](../../notes/execution/2026-09-28-codegen-decision.md) | Proof/evaluator or decision; check status before proposing work. |
| [2026-09-28 · Large callback loops: preparation improves, execution regresses](../../notes/execution/2026-09-28-constant-loop-decision.md) | Proof/evaluator or decision; check status before proposing work. |
| [2026-09-28 · Execution census: first implementation slice](../../notes/execution/2026-09-28-execution-census-implementation.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Faster execution without surprising fallbacks](../../notes/execution/2026-09-28-execution-engines-and-roadmap.md) | Current coverage summary and remaining research boundaries. |
| [2026-09-28 · Fable review: General value representation](../../notes/execution/2026-09-28-fable-architecture-review.md) | Review findings, corrections and remaining concerns. |
| [2026-09-28 · Fable review: callback matrix geometry plan](../../notes/execution/2026-09-28-fable-callback-geometry-review.md) | Review findings, corrections and remaining concerns. |
| [2026-09-28 · Fable review: matrix callback implementation](../../notes/execution/2026-09-28-fable-callback-implementation-review.md) | Review findings, corrections and remaining concerns. |
| [2026-09-28 · Fable review: runtime callback values](../../notes/execution/2026-09-28-fable-callback-runtime-values-review.md) | Review findings, corrections and remaining concerns. |
| [2026-09-28 · Fable review: retained Cholesky-correlation reverse](../../notes/execution/2026-09-28-fable-cholesky-correlation-review.md) | Review findings, corrections and remaining concerns. |
| [2026-09-28 · Fable review of code-generation decision](../../notes/execution/2026-09-28-fable-codegen-review.md) | Review findings, corrections and remaining concerns. |
| [2026-09-28 · Fable review: shared function exits](../../notes/execution/2026-09-28-fable-function-exits-review.md) | Review findings, corrections and remaining concerns. |
| [2026-09-28 · Fable review: keep working in roadmap order](../../notes/execution/2026-09-28-fable-next-runtime-review.md) | Review findings, corrections and remaining concerns. |
| [2026-09-28 · Fable review: normal identity GLM recorder](../../notes/execution/2026-09-28-fable-normal-glm-review.md) | Review findings, corrections and remaining concerns. |
| [2026-09-28 · Roadmap review: findings and dispositions](../../notes/execution/2026-09-28-fable-roadmap-review.md) | Review findings, corrections and remaining concerns. |
| [2026-09-28 · Fable review: Inactive solver scratch](../../notes/execution/2026-09-28-fable-scratch-review.md) | Review findings, corrections and remaining concerns. |
| [2026-09-28 · Fable review: standalone function plan](../../notes/execution/2026-09-28-fable-standalone-function-review.md) | Review findings, corrections and remaining concerns. |
| [2026-09-28 · Fable review: standalone compiled calls](../../notes/execution/2026-09-28-fable-standalone-implementation-review.md) | Review findings, corrections and remaining concerns. |
| [2026-09-28 · Fable review: remaining RNG coverage](../../notes/execution/2026-09-28-fable-vector-rng-review.md) | Review findings, corrections and remaining concerns. |
| [2026-09-28 · For-bound semantics: Fable review](../../notes/execution/2026-09-28-for-bound-semantics-review.md) | Review findings, corrections and remaining concerns. |
| [2026-09-28 · Stan loop semantics, using the existing engines](../../notes/execution/2026-09-28-for-bound-semantics.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Shared compiled function exits](../../notes/execution/2026-09-28-function-exits.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Deferred general-value runtime proposal](../../notes/execution/2026-09-28-general-value-program-decision.md) | Proof/evaluator or decision; check status before proposing work. |
| [2026-09-28 · Interpreter audit: baseline and current disposition](../../notes/execution/2026-09-28-interpreter-gap-audit.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Loop-aware register reverse: correct on the probe, slower in complete solves](../../notes/execution/2026-09-28-loop-adjoint-results.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Continued native execution experiments](../../notes/execution/2026-09-28-loop-history-followup.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Pinned Stan Math expression arguments: two oracle limitations](../../notes/execution/2026-09-28-matrix-callback-upstream-exceptions.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Native integer operations in the existing register engine](../../notes/execution/2026-09-28-native-integer-coverage.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Native kernel profile and next evaluator](../../notes/execution/2026-09-28-native-kernel-profile.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Nested callback arrays in the existing engines](../../notes/execution/2026-09-28-nested-callback-coverage.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Nested loops stay in the existing register engine](../../notes/execution/2026-09-28-nested-loop-coverage.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Runtime boundary and continued native-performance work](../../notes/execution/2026-09-28-next-runtime-design-decision.md) | Proof/evaluator or decision; check status before proposing work. |
| [2026-09-28 · Normal identity GLM: direct partial recording](../../notes/execution/2026-09-28-normal-glm-results.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Fill existing execution-engine gaps before replacing MirInterp](../../notes/execution/2026-09-28-runtime-for-coverage.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Fable review: runtime for-loop coverage](../../notes/execution/2026-09-28-runtime-for-fable-review.md) | Review findings, corrections and remaining concerns. |
| [2026-09-28 · Runtime indexing in compiled callbacks](../../notes/execution/2026-09-28-runtime-index-coverage.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Runtime integer callback arguments in generated quantities](../../notes/execution/2026-09-28-runtime-integer-callbacks.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Runtime solver controls in generated quantities](../../notes/execution/2026-09-28-runtime-solver-controls.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Scalar RNG compiled coverage](../../notes/execution/2026-09-28-scalar-rng-implementation.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Compiled standalone function calls: next bounded slice](../../notes/execution/2026-09-28-standalone-function-plan.md) | Proof/evaluator or decision; check status before proposing work. |
| [2026-09-28 · Compiled standalone functions: implementation and measurements](../../notes/execution/2026-09-28-standalone-function-results.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Structured callback experiment: correct, but slower on the tested solves](../../notes/execution/2026-09-28-structured-callback-results.md) | Dated results, proof limits and linked evidence. |
| [2026-09-28 · Remaining RNG name and context coverage](../../notes/execution/2026-09-28-vector-rng-implementation.md) | Dated results, proof limits and linked evidence. |
| [2026-09-29 · Execution coverage: pre-merge code and benchmark audit](../../notes/execution/2026-09-29-execution-code-cleanup.md) | Removed code, retained benchmarks and validation. |
| [2026-09-29 · Container RNG arguments in the existing engines](../../notes/execution/2026-09-29-container-rng-results.md) | Values/stream/error parity and native measurements; setup tradeoff accepted for normal use, not yet merged. |
| [Integer-expression results](../../notes/execution/2026-09-29-integer-expression-results.md) | Existing register division/remainder and proved array-literal reductions; independent oracle and normal-use tradeoff. |
| [Integer-expression measurements](../../notes/execution/data/2026-09-29-integer-expression-performance.json) | Raw paired native phase timings, source/binary identities and canaries. |
| [Standalone nested-container results](../../notes/execution/2026-09-29-standalone-container-results.md) | Prepared layout adapters and proved no-op returns; correctness, native gains, rejected roundtrips and remaining contracts. |
| [Standalone container measurements](../../notes/execution/data/2026-09-29-standalone-container-performance.json) | Paired native calls, independent CmdStan layout oracle and rejected iterations. |
| [Scalar integer initialization](../../notes/execution/2026-09-29-uninitialized-int-results.md) | Final corpus output refusal, sentinel semantics, independent oracle and native timings. |
| [Scalar initialization measurements](../../notes/execution/data/2026-09-29-uninitialized-int-performance.json) | Paired native phases, normal-use canaries and binary identities. |
| [Recorded-corpus output coverage](../../notes/execution/data/2026-09-29-output-coverage.txt) | All 329 recorded models select complete compiled outputs after the scalar-initialization fix; other MIR roles are outside this report. |
| [Local-storage design checkpoint](../../notes/execution/2026-09-29-local-storage-checkpoint.md) | Proposed closed bounded regions before local MIR; scope, proof obligations and evaluation plan. |
| [Fable next-steps review and comparison](../../notes/execution/2026-09-29-fable-next-steps-review.md) | Revised ordering after PR #410: focused context inventory, then direct admission or bounded-region probe; verbatim review, source corrections and deferred work. |
| [Local fallback opportunity measurements](../../notes/execution/data/2026-09-29-local-fallback-opportunity.json) | Whole-output MIR versus fixed-storage source control; potential savings, not an implemented boundary. |
| [2026-09-29 · Execution coverage: working checklist](../../notes/execution/2026-09-29-execution-coverage-checklist.md) | Hierarchical implementation queue: known gaps, direct extensions, local fallback experiments, design checkpoints and performance/correctness gates. |
| [2026-09-29 · Remaining uses of the MIR interpreter](../../notes/execution/2026-09-29-mir-interpreter-remaining-uses.md) | Source audit after #407/#408: production entry points, remaining refusals, existing-engine opportunities and diagnostic limits. |
| [2026-09-29 · Local fallbacks and a route to broad Stan coverage](../../notes/execution/2026-09-29-local-fallback-and-coverage-strategy.md) | Strategic proposal: contain fallbacks, extend existing engines, define compatibility and performance gates. |
| [2026-09-29 · Fable review: local fallback and long-term coverage](../../notes/execution/2026-09-29-fable-local-fallback-review.md) | Read-only strategic review, lead's source corrections and full returned text. |

## Loops and ctsem

| Document | Contents / use |
| --- | --- |
| [2026-08-29 · ctsem/stanc3 follow-up for stanli issue #248](archive/plans/2026-08-29-ctsem-stanc3-results.md) | Historical implementation/measurement record. |
| [2026-08-29 · Loop-bearing regions: bounded-memory scans for sequential models](archive/plans/2026-08-29-loop-bearing-regions.md) | Historical plan or digest; read final disposition first. |
| [2026-09-03 · Structured loops: static-storage research digest](archive/plans/2026-09-03-structured-loop-static-storage.md) | Historical plan or digest; read final disposition first. |
| [2026-09-19 · ctsem memory and throughput: frames and recording digest](archive/plans/2026-09-19-ctsem-memory.md) | Historical plan or digest; read final disposition first. |
| [2026-09-20 · Recording-site metadata experiment](archive/plans/2026-09-20-ctsem-index-recording.md) | Historical plan or digest; read final disposition first. |
| [2026-09-20 · ctsem performance after PR392](archive/plans/2026-09-20-ctsem-post-392.md) | Historical plan or digest; read final disposition first. |
| [2026-09-20 · Fable 5.1 follow-up proof audit](archive/plans/2026-09-20-ctsem-proof-implementation-review.md) | Historical review; findings and dispositions, not authorization. |
| [2026-09-20 · ctsem follow-up proof packets](archive/plans/2026-09-20-ctsem-proof-packets.md) | Historical plan or digest; read final disposition first. |
| [2026-09-20 · Fable 5.1 follow-up proof audit](archive/plans/2026-09-20-ctsem-proof-review.md) | Historical review; findings and dispositions, not authorization. |
| [2026-09-20 · ctsem recording performance: implementation draft](archive/plans/2026-09-20-ctsem-recording-draft.md) | Historical plan or digest; read final disposition first. |
| [2026-09-20 · Remaining ctsem architecture work](archive/plans/2026-09-20-ctsem-remaining-architecture.md) | Historical plan or digest; read final disposition first. |
| [2026-09-20 · Fable 5.1 data-branch delta review](archive/plans/2026-09-20-ctsem-remaining-dispatch-review.md) | Historical review; findings and dispositions, not authorization. |
| [2026-09-20 · Fable 5.1 audit of remaining architecture](archive/plans/2026-09-20-ctsem-remaining-fable-review.md) | Historical review; findings and dispositions, not authorization. |
| [2026-09-20 · Fable 5.1 implementation review of remaining architecture](archive/plans/2026-09-20-ctsem-remaining-implementation-review.md) | Historical review; findings and dispositions, not authorization. |
| [2026-09-20 · Fable 5.1 review of ctsem frames and first-gradient architecture](archive/reviews/2026-09-20-ctsem-fable.md) | Historical review; findings and dispositions, not authorization. |
| [2026-09-20 · Fable 5.1 review of post-392 execution plan](archive/reviews/2026-09-20-ctsem-post-392-fable.md) | Historical review; findings and dispositions, not authorization. |
| [2026-09-20 · Fable 5.1 review of post-392 solve implementation](archive/reviews/2026-09-20-ctsem-solve-implementation-fable.md) | Historical review; findings and dispositions, not authorization. |
| [2026-09-20 · Deferred research: ordinary-model timing and RSS controls](../../notes/performance/2026-09-20-ctsem-ordinary-model-controls.md) | Dated results, proof limits and linked evidence. |
| [2026-09-20 · ctsem frame proofs and startup reuse](../../notes/performance/2026-09-20-ctsem-proof-packets.md) | Dated results, proof limits and linked evidence. |
| [2026-09-20 · ctsem recording-site results](../../notes/performance/2026-09-20-ctsem-recording-sites.md) | Dated results, proof limits and linked evidence. |
| [2026-09-20 · Remaining ctsem architecture results](../../notes/performance/2026-09-20-ctsem-remaining-architecture.md) | Dated results, proof limits and linked evidence. |
| [2026-09-20 · ctsem solve-kernel results after PR392](../../notes/performance/2026-09-20-ctsem-solve-kernels.md) | Dated results, proof limits and linked evidence. |

## Compiler and preparation

| Document | Contents / use |
| --- | --- |
| [Stanli portable MIR v2](../../compiler/portable_ir/SCHEMA.md) | Portable MIR producer/consumer format and compatibility contract. |
| [2026-08-06 · Elementwise-lp densities and batched mixture kernels](archive/plans/2026-08-06-elementwise-lp.md) | Historical plan or digest; read final disposition first. |
| [2026-08-06 · Perf Phase 2 Plan](archive/plans/2026-08-06-perf-phase2.md) | Historical plan or digest; read final disposition first. |
| [2026-08-06 · Tape islands: compile irreducible scalar residue to one op](archive/plans/2026-08-06-tape-islands.md) | Historical plan or digest; read final disposition first. |
| [2026-08-08 · Native adjoint program: differentiate the register machine without vars](archive/plans/2026-08-08-native-adjoint-program.md) | Historical plan or digest; read final disposition first. |
| [2026-08-09 · One vocabulary: the register machine calls the graph's kernels](archive/plans/2026-08-09-kernel-call-instruction.md) | Historical plan or digest; read final disposition first. |
| [2026-08-25 · Handoff: lessons from the stanc3 vectorize_loops work that apply to stanli](archive/plans/2026-08-25-stanc3-vectorize-lessons.md) | Historical plan or digest; read final disposition first. |
| [2026-08-26 · OCaml MIR backend rollout](archive/plans/2026-08-26-ocaml-mir-backend-rollout.md) | Historical plan or digest; read final disposition first. |
| [2026-08-28 · Generic destination forwarding: implementation and results](archive/plans/2026-08-28-destination-forwarding-results.md) | Historical implementation/measurement record. |
| [2026-08-28 · ODEs beyond CmdStan: compiled sensitivity design](archive/plans/2026-08-28-ode-beyond-cmdstan.md) | Historical plan or digest; read final disposition first. |
| [2026-08-28 · ODE RHS generated-adjoint experiment: Phase 0 result](archive/plans/2026-08-28-ode-rhs-generated-adjoint-results.md) | Historical implementation/measurement record. |
| [2026-08-28 · ODE sensitivity ceilings after Phase 1](archive/plans/2026-08-28-ode-sensitivity-ceiling-results.md) | Historical implementation/measurement record. |
| [2026-08-29 · Direct compiled RK sensitivities: production result](archive/plans/2026-08-29-ode-direct-rk-results.md) | Historical implementation/measurement record. |
| [2026-08-30 · Finalized integer-payload compaction](archive/plans/2026-08-30-idata-compaction-results.md) | Historical implementation/measurement record. |
| [2026-08-30 · Bind-only Op scratch layout](archive/plans/2026-08-30-op-scratch-layout-results.md) | Historical implementation/measurement record. |
| [2026-09-09 · Where the passes were too narrow for vectorized MIR](archive/plans/2026-09-09-vectorize-consumption-generality.md) | Historical plan or digest; read final disposition first. |
| [2026-09-10 · Prep-time tuning: measure where the estimate cannot decide](archive/plans/2026-09-10-prep-time-tuning.md) | Historical plan or digest; read final disposition first. |
| [2026-09-11 · Carver boundaries: one cost function for island, split, and leave](archive/plans/2026-09-11-carver-boundaries.md) | Historical plan or digest; read final disposition first. |
| [2026-09-11 · Lane layout unification for reroll and partition](archive/plans/2026-09-11-lane-layout-unification.md) | Historical plan or digest; read final disposition first. |
| [2026-09-11 · Shared executor data, native matrix pullbacks, and streaming JSON](archive/plans/2026-09-11-shared-data-native-pullbacks.md) | Historical plan or digest; read final disposition first. |
| [runtime/include/stanli](../../runtime/include/stanli/README.md) | Public C++ header map. |
| [Graph optimizations and performance work](../../runtime/src/OPTIMIZATIONS.md) | Production optimization proofs, refusal boundaries and historical evidence. |
| [runtime/src](../../runtime/src/README.md) | Runtime source ownership and execution components. |

## Numerics and performance history

| Document | Contents / use |
| --- | --- |
| [2026-08-08 · Competing with CmdStan: the seven-item roadmap](archive/plans/2026-08-08-cmdstan-parity-roadmap.md) | Historical plan or digest; read final disposition first. |
| [2026-09-11 · Harness execution gates](archive/plans/2026-09-11-harness-execution-gates.md) | Historical plan or digest; read final disposition first. |
| [2026-09-14 · Educational performance investigation](archive/plans/2026-09-14-educational-performance.md) | Historical plan or digest; read final disposition first. |
| [2026-09-14 · Pareto end-to-end parity](archive/plans/2026-09-14-pareto-parity.md) | Historical plan or digest; read final disposition first. |
| [2026-09-15 · Teaching-model performance investigation: retained conclusions](archive/plans/2026-09-15-teaching-performance.md) | Historical plan or digest; read final disposition first. |
| [2026-09-20 · Sampler and generated-quantities RNG alignment](../../notes/performance/2026-09-20-sampler-rng-parity.md) | Dated results, proof limits and linked evidence. |
| [2026-09-20 · Sampler startup alignment](../../notes/performance/2026-09-20-sampler-startup-parity.md) | Dated results, proof limits and linked evidence. |
| [2026-09-21 · Issue 374: numerical agreement before further speed work](../../notes/performance/2026-09-21-issue-374-numerics.md) | Dated results, proof limits and linked evidence. |
| [2026-09-21 · Issue 374: matched 2.40 refresh and GEV profile](../../notes/performance/2026-09-21-issue-374-refresh.md) | Dated results, proof limits and linked evidence. |
| [2026-09-21 · Issue 374: scalar backward experiment](../../notes/performance/2026-09-21-issue-374-scalar-experiment.md) | Dated results, proof limits and linked evidence. |
| [2026-09-21 · Issue 374: retained evidence and next measurement](../../notes/performance/2026-09-21-issue-374-triage.md) | Dated results, proof limits and linked evidence. |
| [2026-09-22 · Full-range slice regression](../../notes/performance/2026-09-22-identity-slice.md) | Dated results, proof limits and linked evidence. |
| [2026-09-22 · Intel macOS oracle for kronecker_gp](../../notes/performance/2026-09-22-intel-oracle.md) | Dated results, proof limits and linked evidence. |

## Parallel reductions

| Document | Contents / use |
| --- | --- |
| [2026-09-17 · Fable review: native reduce_sum parallelism](archive/plans/2026-09-17-native-reduce-sum-fable-review.md) | Historical review; findings and dispositions, not authorization. |
| [2026-09-18 · Native reduce_sum bindings integration](archive/plans/2026-09-18-native-reduce-sum-bindings.md) | Historical plan or digest; read final disposition first. |
| [2026-09-18 · Fable review: compact reduce_sum imports](archive/plans/2026-09-18-native-reduce-sum-import-review.md) | Historical review; findings and dispositions, not authorization. |
| [2026-09-18 · Native reduce_sum: compact imports and gradient publication](archive/plans/2026-09-18-native-reduce-sum-imports.md) | Historical plan or digest; read final disposition first. |
| [2026-09-18 · 2026-09-18-native-reduce-sum-integration-review-followup](archive/plans/2026-09-18-native-reduce-sum-integration-review-followup.md) | Historical review; findings and dispositions, not authorization. |
| [2026-09-18 · 2026-09-18-native-reduce-sum-integration-review](archive/plans/2026-09-18-native-reduce-sum-integration-review.md) | Historical review; findings and dispositions, not authorization. |
| [2026-09-18 · Native reduce_sum integration](archive/plans/2026-09-18-native-reduce-sum-integration.md) | Historical implementation/measurement record. |
| [2026-09-18 · Native reduce_sum feasibility experiment](archive/plans/2026-09-18-native-reduce-sum-prototype.md) | Historical plan or digest; read final disposition first. |
| [2026-09-17 · Native within-chain reduce_sum parallelism](archive/specs/2026-09-17-native-reduce-sum-parallelism.md) | Historical specification; verify current implementation. |

## Packaging and interfaces

| Document | Contents / use |
| --- | --- |
| [Splitting the browser runtime: what was tried and why it is not in](archive/density-pack.md) | Historical plan or digest; read final disposition first. |
| [2026-08-30 · Python Function call overhead](archive/plans/2026-08-30-python-function-overhead.md) | Historical plan or digest; read final disposition first. |
| [2026-08-04 · stanli: a portable Stan runtime](archive/specs/2026-08-04-stan-portable-runtime-design.md) | Historical specification; verify current implementation. |
| [2026-08-10 · BridgeStan ABI facade and sampler strategy](archive/specs/2026-08-10-bridgestan-facade-design.md) | Historical specification; verify current implementation. |
| [2026-08-11 · Embedding the model in the data argument](archive/specs/2026-08-11-embedded-mir-data.md) | Historical specification; verify current implementation. |

## Published artifacts

| Document | Contents / use |
| --- | --- |
| [Corpus benchmark evidence](../../output/corpus-performance-vectorized/README.md) | Published run/report provenance; retains recorded revision. |
| [Corpus benchmark evidence](../../output/corpus-performance/README.md) | Published run/report provenance; retains recorded revision. |
| [Rethinking report evidence](../../output/rethinking-report/README.md) | Published run/report provenance; retains recorded revision. |
| [Rethinking models: speedup and numerical differences](../../output/rethinking-report/rethinking-report.md) | Published run/report provenance; retains recorded revision. |
| [Teaching sweep: evidence and reproduction](../../output/teaching-performance/EVIDENCE.md) | Published run/report provenance; retains recorded revision. |
| [Teaching model performance](../../output/teaching-performance/README.md) | Published run/report provenance; retains recorded revision. |
