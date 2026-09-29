# Local fallbacks and a route to broad Stan coverage

September 29, 2026; source base `df86223160f1387a37ed47c267df1bf31e77f11c`.
Strategic proposal, not an implemented backend or a performance result. The
[remaining-use audit](2026-09-29-mir-interpreter-remaining-uses.md) describes
current behavior. Native performance is the priority; instruction generation
and stencil JIT remain tabled.

The [working checklist](2026-09-29-execution-coverage-checklist.md) is the ordered
implementation queue. It selects direct container-RNG coverage first; the local
boundary experiment below follows the cheap direct closures and measurements.

[Fable's review](2026-09-29-fable-local-fallback-review.md) supports the local-region
direction. Its recommendations and several corrections to comments-based claims
are recorded alongside the full returned review.

## The direction

Keep fast numerical work in the existing graph and register paths. Extend their
coverage where the required behavior already fits. When an operation cannot yet
be lowered, aim to interpret the smallest region with a safe, explicit boundary,
then resume prepared execution. This can contain a performance cliff without
making the unsupported operation itself faster.

An expression is sometimes a sufficient region. A whole statement, branch,
loop, or function may be needed when values, mutations, control flow, or runtime
sizes cross its boundary. “Smallest” means semantically self-contained, not
fewest characters of source. Many tiny crossings may also cost more than one
larger region, so placement needs cost evidence as well as correctness.

An illustrative case is a function that creates a runtime-sized temporary and
returns a scalar sum. A graph can call that function as one opaque operation:
its caller only needs a scalar input/output contract. The temporary can remain
inside the function. If instead the function returns a vector whose length
changes and the caller indexes or resizes it, a fixed-size graph slot is
insufficient. Either contain the vector's consumers in the same region, prove
a safe fixed capacity, or introduce a dynamic-value representation.

## What the current engines can express

| Gap | What is missing | Architectural consequence |
| --- | --- | --- |
| A builtin or overload with fixed-size inputs/results | Handler, registered kernel, type rule, validation or derivative support | Usually extend the existing engine; no general runtime redesign |
| A nested container whose shape is known | Layout/type adapters, even when the numerical operation exists | Preserve logical dimensions and storage order at the boundary |
| A data-dependent branch or loop | Possibly lowering coverage, rather than execution capability | Registers already have jumps; structured loops already preserve control flow |
| Internal dynamic storage with fixed external results | Storage inside a callable region | Can be hidden behind a graph/register call; does not require dynamic slots everywhere |
| Dynamic values that cross region boundaries | Runtime lengths, ownership, allocation, views and lifetime rules | Extend the value/storage contract or enlarge the region |
| General recursion | Call stack, typed frames, returns and dynamic storage lifetime | Not inherently impossible for a register machine; absent from the current inlining-based contract |
| Complex values, tuples, or an absent Stan builtin | End-to-end language representation and mathematical implementation | MIR fallback cannot provide support that MirInterp itself lacks |
| RNG, printing, rejection, target updates | Correct effect ordering and host state | Must remain explicit across engines and compiler optimizations |
| Differentiation through a region | A backward contract and retained values/state | Forward-value compatibility alone is insufficient |

The current graph already contains callable register regions and structured
loops. It is not intrinsically limited to straight-line arithmetic. The present
`Program`, however, uses fixed numbered ranges and inlines functions: it is not
already a complete general-purpose VM. See
[graph-to-program lowering](../../runtime/src/lower_stmt.cpp#L694),
[Program::Call](../../runtime/include/stanli/program.hpp#L205), and
[structured control](../../runtime/include/stanli/structured_loop.hpp#L46).

Stan's types are statically known even when container sizes vary at runtime.
The [Stan reference manual](https://mc-stan.org/docs/reference-manual/types.html)
also includes complex and tuple types. Our
[coverage inventory](../../docs/coverage.md) currently treats complex values and
tuple results as expected unsupported cases; those labels are backlog categories,
not an exemption from the long-term compatibility goal.

## What a local MIR call would need

Select it during preparation after a lowering refusal. Do not execute half a
program, catch an exception, and restart it under MIR: that can repeat RNG
draws, prints, writes, or target contributions, and reinterpret legitimate
Stan rejection as a compiler failure.

Prepare a region description containing:

- The retained MIR and resolved functions; typed inputs and outputs, logical
  shapes, layout conversions, and values that must be written back.
- A conservative account of effects, aliases and control exits. A branch arm
  executes only if selected; an early return or `break` must not escape into a
  caller that assumes normal fallthrough.
- Caller-owned RNG/message state where needed, and exact target read/update
  ordering. Graph scheduling, constant folding, common-subexpression elimination,
  memoization and dead-code removal must respect this contract.
- Private per-invocation storage, including concurrent chains and nested calls.
  Shared immutable MIR does not permit a shared mutable environment.
- A derivative contract for active inputs. The backward pass must receive the
  corresponding forward values and branch history. Stan Math replay or a retained
  tape may provide a bridge, but effect replay, tape lifetime and accumulation
  order must be specified. A dense Jacobian should not be the universal default.

Existing kernel calls offer a useful insertion point, not a complete solution.
They currently have at most six input ranges and fixed output storage. MIR and
graph container layouts are not universally interchangeable. An active opaque
call also needs a backward implementation: the
[register var adapter](../../runtime/src/executor.cpp#L135) will not invent one.
Packing more inputs, returning mutated state, retaining a tape, and representing
dynamic results all require deliberate contracts and measured costs.

Start with value-only, fixed-external-shape, self-contained pure function calls
in generated quantities. Such a call can have dynamic internal temporaries.
Require precise exception placement and a valid continuation even in this narrow
case. This first scope does not solve effectful regions or active solver callbacks.

## Sequence toward CmdStan coverage

1. **Define coverage against a pinned Stan/CmdStan version.** Track each overload
   by types, shapes, block/callback context, value behavior, gradient behavior,
   effects and selected engine. Distinguish unsupported cases, generator gaps,
   numerical mismatches and slow-but-correct cases. Expand the existing signature
   inventory and model corpus rather than introducing another disconnected list.
   Custom external C++ integrations need a separate compatibility contract.
2. **Close the direct gaps.** Container RNGs, graph integer operations already
   supported by registers, and standalone container adapters are concrete
   candidates. Reuse Stan Math and shared metadata across all paths. Compare
   against CmdStan, not only against MirInterp.
3. **Test whether local fallback actually contains a cliff.** Compare an unchanged
   whole-output fallback with a narrowly scoped interpreted call surrounded by
   compiled work. Include both substantial and almost-empty surrounding work,
   repeated invocations and varying container sizes. The latter cases expose
   boundary overhead rather than hiding it behind a large win.
   First use the existing refusal inventory to estimate how much surrounding work
   a closed region would preserve. A statement count can guide selection but
   cannot substitute for timings. Track existing cases so an accidental expansion
   of their fallback is caught; do not prohibit new interpreted regions when they
   add support for a previously rejected model.
4. **Extend region boundaries only with evidence.** Mutation, effects, loops,
   early exits and active derivatives each add contracts. Prefer replacing a
   frequently crossed fallback with a native handler when it fits naturally.
5. **Decide on dynamic Program values and call frames separately.** The likely
   consolidation direction is to add the missing capabilities to the existing
   register machinery while preserving its inexpensive fixed-storage instructions.
   Dynamic handles and frames could live in separate per-call storage, paid for
   only by instructions that use them. This is a hypothesis to test, not a free
   extension or a promise that every current opcode can remain unchanged.
6. **Retire MirInterp only after its remaining contracts are covered.** Include
   preparation probes, transformed data, initialization and test-oracle uses.
   A register VM still dispatches instructions; deleting tree walking does not
   imply eliminating interpretation or matching generated C++ on every workload.

Keep ordinary model startup, first gradient, warm gradients, complete inference,
output generation, memory and binary size in the performance gates. “Reasonably
performant” needs workload-specific budgets, not just a percentage of registered
function names. Universal no-regression guarantees cannot be established from
a finite corpus; preserve fast paths and broaden representative/adversarial tests.

## Alternatives and the first decision

Direct coverage patches alone have the lowest architectural cost, but leave an
all-or-nothing cliff whenever the next unsupported construct appears. A local
MIR call limits the damage and reuses the current interpreter, but crossings,
copies and repeated environments can be expensive. Extending Program to dynamic
values and frames could cover the long tail, but risks creating a second general
interpreter and imposing costs on today's fast paths. It needs a clear migration
and eventual consolidation story.

The strongest objection to local fallback is that it can become permanent glue
that hides pressure to implement efficient operations. Keep the selected region,
reason and boundary costs observable, and retain direct implementation as the
comparator. Do not optimize for the count of removed interpreter calls.

The first proposed design experiment stops after the narrow value-only boundary:
demonstrate correct results, rejection position and output layout against CmdStan;
show the intended compiled/MIR/compiled selection; measure complete output time,
preparation and memory against whole-output MIR plus an unaffected compiled
canary. Continue if a real fallback workload benefits and ordinary cases show
no resolved regression. Revise/coarsen the boundary if conversion dominates;
prefer direct lowering if it is simpler and faster. Park the approach if its
correct form cannot contain a meaningful cliff. This note records the proposed
experiment; no prototype has been implemented or enabled.

Fable suggested the existing `binomial_rng` partial-output-fallback fixture as
the first comparison, including a direct container-RNG handler. That is a useful
follow-up because it tests RNG state as well as performance. Start with the pure
fixed-result helper above to isolate value/layout handoff; use the RNG fixture
when adding effectful boundaries, or close its direct handler first if that is
the cheaper improvement. Neither experiment by itself establishes an active
derivative boundary or improved solver-callback performance.
