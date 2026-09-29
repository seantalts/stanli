# Structured callback experiment: correct, but slower on the tested solves

Do not route these scalar ODE callbacks into the structured-loop engine by
default. Reusing its existing derivative history works, including changing
branches and overwritten values, but loses to the current execution paths in
complete solves. Grouping each iteration's arithmetic into an existing register
segment reduces saved history but does not recover the speed. The production
runtime and unrolling policy are unchanged.

This implements the bounded experiment authorized after the
[large-loop comparison](2026-09-28-constant-loop-decision.md). The branch was
clean and synchronized against fetched `origin/HEAD` at
`6ce2018b5382b459666ab32fe0df50d4a6d30ba4`; the task base was `776b1b0c`.

## What the adapter does

A developer tool compiles a small wrapper around an ordinary Stan callback.
The wrapper exposes time, one state, and one real parameter as unconstrained
inputs, and its scalar result is the callback's only output. Existing graph
lowering retains the function's loop as `OP_LOOP`. An ordinary `Executor`
provides the value and weighted reverse operation. No numerical derivative
rule or solver algorithm is reimplemented.

The complete-solve experiment connects that adapter to the existing developer
benchmark's double-valued coupled RK system. It compares with the same Stan
Math integrator using the current register callback and nested autodiff. A
third arm uses the existing generated register reverse where eligible. Local
values, local Jacobians, complete solution values/derivatives, and the number
of solver callback evaluations must match bit for bit before any timing.

This is a feasibility adapter, not production callback lowering. It supports
one state, one real parameter, fixed geometry, and scalar output. The supplied
wrapper must implement the callback being compared. The tool checks the input
layout and selected loop engine; numerical equivalence is checked against the
original callback before timing. It adds no model-name selection to runtime
code and creates no additional execution engine.

## Experiments and decision

1. **Reuse structured history directly.** Prediction: compact preparation and
   existing reverse kernels could beat nested autodiff. The adapter passed
   correctness checks but complete solves were slower. This rejects immediate
   default routing, not structured loops for all workloads.
2. **Group arithmetic within each iteration.** Existing `compile_segment`
   builds a straight-line register forward/reverse program. A developer-only
   pass combines eligible contiguous calls/aliases, exports values read outside
   the segment, and leaves control/history in the structured engine. It clones
   prepared plans and refuses unsupported runs. This specifically tests whether
   fewer kernel records and calls can rescue the first adapter. Correctness
   passes and history shrinks, but solve time does not beat the incumbent.
3. **Use the existing frame-history option.** A small diagnostic ablation
   selects `STANLI_STRUCTURED_FRAMES=1`. It is slower still: approximately
   888 µs for the 2,048-trip simple solve versus 271 µs for its register oracle,
   and 833 versus 319 µs for the 128-trip branch solve. These are two paired
   batches, not the six-process comparison below. No further frame tuning was
   justified by this result.

The simple callback repeatedly adds `rate * y[1] / N`. The branch callback
changes trip count with time, changes branch with the state sign and iteration,
and overwrites its carried accumulator. The latter matters because local
repeated calls with an unchanged path can look faster than a solver that
changes paths and must rebuild its saved history.

## Native solve timings

Release build, AppleClang 21, Darwin arm64. Six fresh processes per case/path;
each process alternates two paired batches after 50 ms warmup per arm. Batches
contain 20 solves at 8/128 trips and three at 2,048. Builds, tests, and oracle
compilation did not overlap these measurements. These are complete ODE solves,
not whole-model gradients or complete inference.

| Simple-loop trips | Generated register reverse µs | Structured µs | Structured with register segments µs |
| ---: | ---: | ---: | ---: |
| 8 | 1.56 | 2.75 | 2.81 |
| 128 | 12.89 | 27.95 | 30.30 |
| 2,048 | 196.52 | 469.59 | 488.98 |

All columns above use the same double-valued coupled-system benchmark, with
different callback derivative providers. The structured paths also lose to
their paired unrolled-register/nested-autodiff oracle, which takes roughly
18–20 µs at 128 trips and 272–304 µs at 2,048.

| Branch-loop trips | Paired register oracle µs | Structured µs | Paired register oracle µs | Structured with segments µs |
| ---: | ---: | ---: | ---: | ---: |
| 8 | 50.92 | 73.38 | 46.28 | 70.44 |
| 128 | 318.66 | 451.42 | 297.27 | 422.48 |
| 2,048 | 5,003.52 | 7,305.58 | 4,880.05 | 6,470.67 |

The two oracle columns are the controls paired with each candidate, not
different implementations. Avoid attributing their timing variation to code.
Medians, median absolute deviations, and every sample are retained in the JSON
artifact. The 8-trip case checks the ordinary-small-loop cost.

Compact code still needs derivative storage. For the simple callback at
2,048 trips, reported frozen-history and associated state pools use 837,436
bytes without grouping and 623,332 with grouping. At 128 trips they use 54,076
and 39,652 bytes. These measured pools grow by about 408 and 304 bytes per
additional iteration respectively. They are not total retained model memory
or process RSS; allocation categories outside the diagnostic are excluded.
The local tool separately records cold wrapper preparation, first gradients,
warm gradients, value-only calls, and outer executor storage. Its register
preparation metric starts from an already parsed function, so comparing those
two preparation numbers as if they had the same boundary would be misleading.

## Correctness and the numerical limit

- 1,728 local callback checks pass bitwise values and all three weighted input
  gradients, including zero/negative/non-unit weights, changing branches,
  zero/one/many trips, value-only calls interleaved with reverse, and copies
  made after execution. Both structured variants are exercised. The compiler
  eliminates the constant zero-trip loop; zero trips through a runtime bound
  are tested with the branch callback.
- Complete RK solves match the register oracle bitwise, including local
  Jacobians and callback counts. All timed cases check point 0; 24 additional
  checks cover points 1 and 2 for both candidates at all three sizes.
- Three permanent CmdStan recordings cover 54 density/gradient/output values
  at their default sizes, within 1 ULP. Wrapper reference tests force the
  structured engine. The ODE recording tests the existing register path.
- A broader CmdStan comparison covers 246 values at several sizes and points.
  **One gradient fails the 10-ULP target:** the branch ODE at N=2,048, point 1
  differs by 24 ULP (absolute error `4.163336342344337e-17`). CmdStan gives
  `-0.0086632040292135386`; the current register path gives
  `-0.0086632040292135802`. The experimental solve and current register solve
  agree bitwise there. This therefore predates the adapter; its numerical
  cause has not been established. No tolerance was widened. The large branch
  timings are diagnostic, not proof that this case meets the external target.
- All **309 CTests pass**. No shipped runtime code changes, so the full corpus
  and installed Python/R suites from the preceding runtime milestone were not
  repeated for this developer-tool experiment.

The original `bench_ode_ceiling` has a historical generic `fvar` adapter that
no longer compiles against several newer Program instructions. The new target
shares its solver harness but excludes that adapter. This experiment neither
repairs nor silently claims validation of the historical fvar path.

## Next architectural choice

Park this wrapper as a default scalar-callback strategy. Its saved-history
cost and path-change handling outweigh the numerical work in these examples.
The negative result does not justify deleting either the interpreter or the
structured engine, and does not rule out reuse for substantial matrix kernels.

The next plausible direction is loop-aware generated reverse within the
existing register engine. It would retain the values and branch decisions
needed by each iteration, then run the existing derivative instructions in
reverse iteration order. That is a new lifetime/history contract, not a switch
to enable existing acyclic reverse. It must handle overwritten values, dynamic
trip counts, break/continue, early returns, and nested calls before broad use.
The structured engine's correctness tests and storage experience can guide it;
the measurements here do not establish its speed or memory cost.

That is the design checkpoint. Keep existing production choices while deciding
whether that larger derivative extension is the next priority. The shared
24-ULP stress discrepancy also remains a separate numerical investigation.

## Reproduction

Build `bench_structured_callback` and `bench_structured_callback_local` in a
Release configuration. Then run:

```sh
python3 tools/bench_structured_callback.py --build build-release --output /tmp/structured-callback-replay
build-release/bench_structured_callback_local tests/fixtures/structured_callback_branch.tmir.sexp 128
STANLI_CALLBACK_PROBE_SEGMENTS=1 build-release/bench_structured_callback_local tests/fixtures/structured_callback_branch.tmir.sexp 128
```

`STANLI_STRUCTURED_LOOP_DIAGNOSTICS=1` enables the existing history-size report.
Do not use diagnostic timings for performance claims. Source hashes, raw
samples, local results, numerical exceptions, and memory pools are preserved
in `2026-09-28-structured-callback-performance.json`. Build/test logs, independent
oracle output, and per-process timing logs are in
`/tmp/stanli-structured-callback`.
