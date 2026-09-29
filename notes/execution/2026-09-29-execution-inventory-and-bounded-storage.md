# Execution inventory and bounded-storage probe

September 29, 2026. Synced to `dacfaae17cb6ca8d18bc1df678c4a8843f8a7b9f`
(merged PR #410). Integration `948b83f3` preserved the later Fable review;
four documentation conflicts were resolved, with no runtime conflicts.

## What the inventory changes

No simpler real-model callback refusal emerged. Continue toward bounded local
storage through the existing structured engine, after fixing a correctness
issue found by the probe. This is not evidence for a new evaluator or a local
MIR-call boundary.

The [selection records and scoped traces](data/2026-09-29-execution-selection.json.gz)
use the shared corpus inventory, the existing execution benchmark manifest,
solver fixtures with data, and explicit empty-data fallback fixtures. They
retain source/data/MIR hashes, refusals and binary identities. No new census
framework was added.

| Scope | Result |
| --- | --- |
| 329 recorded models, including 11 language fixtures | All select compiled output graphs. |
| Additional, unrecorded `sir` model | Selects a compiled output graph and register ODE callback. Runtime points tested here reject tiny negative ODE states; this is not a numerical coverage pass. |
| Corpus solver callbacks | Four ODE models, each with one selected callback site in log density and one in output; all register programs. |
| 43 focused execution/solver fixtures | Four whole-output fallbacks; 45 interpreted callback sites across model and output graphs. Every callback refusal concerns runtime-sized local storage. These are deliberate conformance cases, not application-frequency measurements. |

The recorded ODE models are `lotka_volterra`, `one_comp_mm_elim_abs` and
`soil_incubation`. Scoped traces at all-zero and all-0.1 unconstrained inputs
show no MIR entries during their gradients or compiled outputs. Preparation
still constructs MIR interpreters and performs folding probes. The deliberate
legacy ODE fallback fixture records 14 and 28 interpreter constructions per
gradient at those inputs, confirming that the trace sees repeated callbacks.
These counts are not exclusive timings. The corpus contains no application
example of the other four solver families, so their fixture coverage must not
be presented as broad workload evidence.

Whole-output fallback fixtures are `execution_rng`, `execution_rng_reject`,
`gq_partial_fallback` and `matrix_callback_value_fallback`. The first two include
effects inside the dynamic-storage region; the last includes a solver. They
are outside the proposed first pure-block contract. The inventory does not
exhaust standalone signatures, transformed-data costs or initialization work.

## What the probe found and what changed

A one-trip loop, preserved with O0 MIR and
`STANLI_STRUCTURED_LOOPS=force`, lets the existing structured engine own a
one-or-two-element temporary and return its scalar sum. This is an isolated
feasibility wrapper, not a proposed production selector.

The initial probe compiled but failed on its valid one-element path:
`size(a)` had been replaced with the allocated capacity, 2. The subsequent
indexed write correctly rejected index 2 against the current length, 1.

Two compile-time shape paths now refuse to substitute capacity for logical
extent: `eval_int` and `try_static_view`. The existing runtime shape-query path
then supplies the declaration's extent snapshot. A narrow invariant-bound proof
allows `for (i in 1:size(a)) a[i] = ...`: indexed writes cannot resize `a`.
Whole assignments, redeclarations, element-dependent bounds and effects do not
qualify for that proof. This changes no engine or automatic root-selection
policy.

The production regression `structured_local_shape` exercises the automatic
32-trip selector, lengths 0/1/2, repeated changing inputs, log density,
gradients and output. The new independent CmdStan reference checks 12 values
at three points, all exactly equal. The 315 native tests and the full recorded
329-model replay pass (1,020,194 values). The corpus's existing 7,040-ULP
cancellation exception remains; these results do not claim a universal 10-ULP
bound.

Ten isolated probes compare repeated output calls with forced MIR, including
RNG draws before and after the temporary. Nine select the structured engine
and agree on values, error status and subsequent RNG state: sum, empty array,
extent snapshot after size-variable mutation, constant read/write bounds,
dynamic read bounds, negative size, partial initialization and nested control.
The tenth, a whole assignment from fixed to runtime shape, refuses and stays
in MIR. These checks do not establish safety for every length-sensitive
operation or rank.

## Performance and remaining design gate

The [raw probe and phase measurements](data/2026-09-29-bounded-storage-probe.json.gz)
retain six alternating fresh-process pairs/triples, source/input data, hashes,
all phase samples, and the before/after edge probes. Native Release, Darwin
arm64, threads enabled, full LP. The baseline library is the #410 runtime
(`c1020a38…`, 39,831,040 bytes); candidate `6eb58e55…` is 288 bytes larger.
The embedded build label is not the sole source identity: the baseline was
built on the pre-merge branch, and the records retain the actual binary and
changed-source hashes. No dependency or execution engine was added.

For a proper-prior variant of the small temporary-array example, the same
candidate library runs either forced whole-output MIR or the forced structured
wrapper. Binomial outputs follow the pure temporary region. Medians:

| Following outputs | Preparation, MIR → structured (µs) | First output (µs) | Warm output (µs) | Preparation + short inference (ms) |
| --- | ---: | ---: | ---: | ---: |
| 1 | 300.9 → 274.3 | 41.1 → 82.0 | 6.50 → 0.64 | 1.588 → 1.407 |
| 32 | 299.0 → 280.1 | 31.3 → 55.9 | 8.32 → 2.26 | 4.118 → 3.951 |
| 1,024 | 410.4 → 383.2 | 96.7 → 107.5 | 74.60 → 54.94 | Not measured |

Short inference includes 100 warmup iterations, 100 posterior draws and their
output rows. Fixed-branch direct controls take 0.62, 2.27 and 56.08 µs per warm
row; their preparation-plus-inference totals are 0.900 and 3.473 ms for the two
small sizes. Thus the structured wrapper nearly matches fixed branches on
repeated identical inputs, but its complete-run benefit over MIR is only about
11% and 4%, not the 10× and 3.7× warm-row ratios. Changing execution paths and
first-use costs matter. First output is slower, and the synthetic workload is
not proof of an application bottleneck. Peak whole-process RSS spans roughly
28–30 MB across the probe variants; this is not retained-region memory.

Three ordinary canaries compare the baseline and corrected libraries using
their normal selection policy. Preparation-plus-short-inference medians are
8.539→8.512 ms (`ar1`), 72.070→71.813 ms (`ode_nested_for`), and 2.054→2.090 ms
(`gq_container_rng_complete`). The last difference is 1.8%, within the observed
inference dispersion (candidate MAD 0.039 ms); paired measurements likewise do
not show a clear ordinary-use regression. All sampled log densities and
gradients agree exactly. This finite set cannot guarantee every model's
performance. The new automatic-path fixture was added to the existing phase
benchmark manifest and smoke-tested; no experimental benchmark backend ships.

After the fix, all 373 surveyed model/fixture engine selections remain the
same. The shape regression adds correct execution through an already selected
engine; the probe flags and one-trip wrapper do not enter production code.

Before admitting ordinary blocks, specify and review:

1. A selector for an existing closed block whose runtime-sized local cannot
   leave it; all exported values must have fixed type and shape. Start with
   pure, value-only generated quantities. Preserve the current path for blocks
   that already compile.
2. A checked aggregate capacity budget, including temporary copies and fills,
   with an ordinary-path fallback when the bound is too loose. A finite integer
   maximum alone is not a memory or profitability policy.
3. A consumer audit for the admitted operations: logical bounds, empty arrays,
   assignment shapes, declaration-time validation and initialization. This
   experiment demonstrated why existing metadata is insufficient as a blanket
   safety proof. Vector orientation, `dims`, additional ranks and whole-array
   assignment need their own checks before inclusion.
4. Transactional compilation and once-only errors/effects. A preceding RNG or
   message must not run again after a runtime failure. Do not retry in MIR.
5. Native preparation, complete output/inference and memory comparisons, plus
   graph/register/structured canaries. The measured wrapper is not production
   admission, and the fixture is not a demonstrated application bottleneck.

The [storage checkpoint](2026-09-29-local-storage-checkpoint.md) retains the
larger contract and alternatives. Local MIR calls and general dynamic frames
remain deferred; the inventory supplied no new workload reason to build them.
