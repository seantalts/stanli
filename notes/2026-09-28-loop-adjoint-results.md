# Loop-aware register reverse: correct on the probe, slower in complete solves

Keep the production callback routing unchanged. Two developer experiments
reuse the register engine's existing forward and derivative rules across
loops. Both pass the measured bitwise comparisons, but neither is a performance
improvement. This closes the bounded feasibility experiment; it does not prove
that all loop-aware generated reverse designs would be slow.

The starting task commit was `8c00cc6e`, synchronized with fetched `origin/HEAD`
`6ce2018b5382b459666ab32fe0df50d4a6d30ba4`. Native Release build, AppleClang 21,
Darwin arm64. No instruction-generation or stencil-JIT work was done.

## How the replacement works

The current register callback executes its instructions using Stan autodiff
variables. Stan records the arithmetic and computes derivatives in reverse.
The experiment instead divides the register program into straight-line blocks
at branches and loop jumps. Each block gets an ordinary double-valued forward
program and a reverse program from the existing `gen_adjoint` generator.

An overwritten register can hold many different values during one loop. The
probe gives each definition a distinct history slot. A copy keeps the same
identity as its source, even when it crosses an iteration boundary. That last
point matters: collecting a separate gradient for each block and adding those
results afterwards can change floating-point accumulation order.

Forward execution saves the values used by each visited block. Reverse execution
walks the actual history backwards, applying the existing derivative rules to
shared adjoint cells. Branch decisions are represented by the recorded path;
reverse does not guess the path or reuse a previous call's decisions.

Two storage designs were tested:

- **Expanded instructions:** rebind and append the derivative instructions for
  every block visit. One call to the existing reverse evaluator processes the
  resulting stream. This is the retained, excluded-from-default-build probe.
- **Mapped block visits:** keep derivative code immutable, record block visits
  and value bindings, and apply the same derivative rules through mapped
  adjoint indices. This reduces history bytes but adds indirect accesses and
  a reverse evaluator invocation per visited block. Its experimental runtime
  changes are preserved as an unapplied patch under `tools/experiments/`.

Neither design adds production admission or a new numerical derivative rule.
The probe accepts fixed-storage scalar arithmetic, comparisons, integer
arithmetic, copies, constants, and jumps. It refuses kernel calls, effects,
dynamic containers, and unsupported scalar rules. Integer division/modulus use
the canonical forward operation and an existing zero-derivative integer rule.
This is deliberately narrower than a general callback implementation.

## Complete native ODE solves

Six fresh processes per callback/size/design. Each process performs two
counterbalanced paired batches after 50 ms warmup per arm: 30 solves at 8/128
iterations and three at 2,048. The order of the two designs alternates between
processes. No builds or other benchmarks ran concurrently. Timings include the
complete ODE solve, but exclude source compilation and model preparation.
They are not full model gradients or inference measurements.

Each process requires bitwise local values/Jacobians, bitwise complete solution
values/Jacobians, and equal solver callback counts before timing. All 72
processes passed those gates.

| Callback | Iterations | Paired current µs | Expanded history µs | Paired current µs | Mapped history µs |
| --- | ---: | ---: | ---: | ---: | ---: |
| Simple retained loop | 8 | 2.89 | 4.19 | 2.90 | 4.82 |
| Simple retained loop | 128 | 31.15 | 56.21 | 28.28 | 59.29 |
| Simple retained loop | 2,048 | 426.22 | 817.68 | 475.96 | 999.63 |
| Branching retained loop | 8 | 51.14 | 75.26 | 51.18 | 103.73 |
| Branching retained loop | 128 | 309.54 | 496.75 | 294.45 | 686.70 |
| Branching retained loop | 2,048 | 5,107.98 | 8,433.80 | 4,905.95 | 10,734.51 |

The current columns are paired controls, not different implementations. Both
callbacks already use compact register control flow. These results should not
be compared directly with the prior experiment's unrolled simple callback:
that generated reverse remains considerably faster and is unchanged.

The [JSON artifact](2026-09-28-loop-adjoint-performance.json) records all process samples, medians and median absolute
deviations. The local phase measurements are exploratory single-process runs,
not the six-process performance verdict. They separate preparation from MIR,
first gradient on the prepared probe, warm forward/history construction, and warm complete callback
gradient. For the mapped simple loop at 128 iterations, recording alone took
about 6.60 µs versus 4.03 µs for the current complete callback gradient, and the
mapped complete gradient took 8.79 µs. Thus saving derivative-code copies did
not remove enough forward cost. This is a timing decomposition, not a CPU
profile proving which individual bookkeeping operation dominates.

Memory diagnostics count live elements of the history's vectors, not reserved
capacity, allocator overhead, Stan's arena, total model memory, or process RSS.
Mapped simple-loop history at 2,048 iterations used 565,916 such bytes; branching
history used 902,012. Compact prepared code still requires history that grows
with the executed work. No package size or installation improvement is claimed;
the final runtime sources and headers are unchanged.

## Correctness and limits

Both designs passed 864 weighted local callback comparisons each, including
changed time/state signs, changed parameters, loop counts 1/8/128/2,048, and
zero iterations for the branching callback. These compare outputs and time,
state, and parameter derivatives bit for bit with the existing register/Stan
Math replay. Copies of warmed workspaces are interleaved with changing paths.

Additional Program-level checks cover aliasing outputs, overwrites, changing
branches, zero/one/many iterations, multiple weighted outputs, repeated reverse
passes, unsupported-operation refusal, invalid jumps/constants, and invalidation
and recovery after a failed forward. The two designs each pass 48 weighted
alias-history cases per local invocation. Refusal does not mutate the canonical
input program. These are feasibility checks, not a proof for arbitrary Stan
programs or all non-finite scalar cases.

The independent CmdStan fixtures and the earlier live stress comparison remain
the external evidence. This experiment adds no live CmdStan regeneration. The
previously recorded 24-ULP full-model gradient discrepancy at branch N=2,048,
point 1 remains unresolved. Bitwise agreement with the current implementation
cannot establish a universal 10-ULP CmdStan bound. No tolerance was widened.

Final validation: all 309 CTest cases passed after restoring the original
runtime. Additional final expanded-history solver checks passed exact gates
for RK45 at point 2 and CKRK at point 1 on the branching fixture. The mapped
patch passes an apply check. Installed-interface and broad corpus runs were
not repeated because the delivered change is confined to developer tools and
documentation, with no runtime source/header changes.

## Reproduction and next decision

Build `bench_loop_adjoint` and `bench_loop_adjoint_local` in Release mode.
`bench_loop_adjoint_local MIR N` runs the local checks and reports phase data.
`python3 tools/bench_loop_adjoint.py --output /tmp/loop-adjoint-results` runs the
six-process complete-solve comparison for expanded history.

To reproduce the mapped variant in a separate checkout, apply
`tools/experiments/loop-adjoint-mapped.patch`, clean-build the same two targets, and
save the resulting benchmark binary separately. The runner's `--mapped-bin`
option compares it with an unmodified expanded-history build. The patch changes
runtime code for the experiment; it is intentionally not applied in the final
worktree. Restore/rebuild that checkout before using it as a production build.

A plausible next design would preserve ordinary forward register execution
and save only the primal values reverse actually needs, rather than materializing
and rebinding a whole local register frame on every block visit. It must still
preserve alias identities, overwritten values, exception recovery, and exact
accumulation order. That needs a separate lifetime analysis and storage contract.
The present measurements justify investigating that cost; they do not yet
justify production routing or removing the interpreter.
