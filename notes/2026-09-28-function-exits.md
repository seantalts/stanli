# Shared compiled function exits

Current authorization: keep implementing interpreter-gap work until completion
or a consequential choice requiring user input. A completed slice or ordinary
implementation design is not such a stop. Native performance is primary;
native instruction generation, stencil JIT and dispatch-JIT remain tabled.
Synced origin/HEAD: 5b913866768f351f86eed4d5a7246414530f3b2d. Baseline 21937576.

## Bounded implementation and evaluator

Replace compile-time unwinding for runtime returns with a per-function result
and explicit jumps to a shared epilogue. Each returned value must have the
same logical view, including dimensions. Nested calls own separate exits;
loop break/continue still target their own loops. Keep a single straight-line
return as an alias with no result copies or extra jump. Leave runtime-sized
return storage and recursive call frames out of this slice, retaining refusal.

The compiler needs to distinguish a terminated lexical path from function
compilation completion: a returned arm does not eliminate its alternate arm,
a loop may execute zero times, and continue can reach another unrolled trip.
No statement after an executed return may run, and only executed return
expressions may allocate numerical work, draw randomness, print or reject.

Use the newly guarded callback cases as positive fixtures, plus nested calls,
zero-trip loops, return/continue/break combinations, mismatched logical return
shapes, and effects. Compare values and weighted adjoints with the established
interpreter; use independent CmdStan model references at three points. Check
selected callback engines so interpretation cannot mask admission failures.
Measure public-C-API phases on a callback model with the saved baseline and
ordinary scalar/AR1 canaries; preserve unchanged straight-line bytecode.
Run full native tests, the complete recorded corpus and installed interfaces
before integrating. Reassess the remaining backlog from actual findings.

## Implementation and correctness results

`ProgramCompiler::function_body` now owns a scoped result range and patches all
runtime return edges to its epilogue. Both callback entry adapters and nested
UDF inlining use it. Lexical path completion is separate from compiler
completion; loops finish patching breaks/continues even when another path
returns. The obsolete paired-terminal-return shortcut was removed. A single
unconditional return still aliases its result; the straight-line callback
retains six instructions without initializer/copy bookkeeping.

The differential evaluator caught an additional existing defect: a pending
break/continue in an unrolled loop could bypass a syntactically unconditional
return and leave jumps targeting instruction zero. The return frame now records
its local loop base and checks pending jumps before choosing the straight-line
path. Fable independently identified the same defect in the initial diff; see
[review and dispositions](2026-09-28-fable-function-exits-review.md).

Validation on the corrected candidate:

- All 275 configured CTests passed after rebuilding affected runtime and test
  consumers. The subsequently added `ode_branch_returns_reference` also passes
  (276 tests now configured).
- Callback values and weighted y/theta gradients agree bitwise with the
  established interpreter, including negative states and both sides of guards.
  General generated reverse also agrees on acyclic early-return programs.
  This does not broaden the separate exact direct-RK whitelist.
- Seeded early-return RNG tests compare complete rows and subsequent engine
  state, with strict tracing forbidding MIR entry during compiled execution.
  Print and unreachable reject checks preserve effect order.
- New CmdStan 2.40 references: generated quantities 21 values, worst 1 ULP;
  mixed loop exits ODE 15 values, 0 ULP; simple branch returns ODE 15 values,
  0 ULP. Each reference covers three points and all per-draw outputs. The mixed
  fixture reaches continue, break and return paths across those points.
- Complete recorded corpus replay: 329/329 models at three points, 1,020,194
  values. All 124 per-model ULP gates pass; worst corpus scaled error 9.38e-13
  (7040 ULP). This is not a universal 10-ULP claim.
- Installed Python and R interface suites pass without skips.

Return storage still requires one fixed logical view. Runtime-sized results,
recursive call frames, and void statement-function calls remain outside this
slice. Rejection-only terminal paths can conservatively refuse when no result
shape is available. Runtime loops still use var replay where generated reverse
cannot retain their iteration history.

## Native performance evaluator

The old baseline 21937576 hangs on the mixed loop-exit fixture because of its
unpatched-jump defect. That run was terminated and excluded; an incorrect or
nonterminating baseline cannot establish a speed ratio. The separate
`ode_branch_returns` fixture uses runtime-guarded returns that the baseline
safely refuses and interprets. Both versions execute the same fixture and
public C API phases. The mixed fixture remains a correctness regression.

Six alternating fresh-process pairs per model through `tools/bench_rng_paths.py`
used the same Release Apple Clang 21 arm64 build configuration. Warm phases use
200 ms warmup plus 250 ms sampling; inference is 100 warmup plus 100 samples
and their output rows. ctypes overhead is included. [Raw measurements and
median/MAD summaries](2026-09-28-function-exits-performance.json) retain hashes.

For the branch-return callback fixture (microseconds, median ± MAD):

| Phase | Baseline | Compiled exits |
| --- | ---: | ---: |
| Warm source compilation | 538.2 ± 4.7 | 514.6 ± 9.1 |
| Preparation from MIR | 207.3 ± 1.8 | 296.8 ± 4.0 |
| First gradient | 302.4 ± 3.3 | 40.4 ± 1.1 |
| Warm gradient | 222.5 ± 6.4 | 2.60 ± 0.07 |
| First output row | 264.5 ± 13.1 | 22.4 ± 1.4 |
| Warm output row | 214.9 ± 1.4 | 1.785 ± 0.016 |
| Short inference and outputs | 363,701 ± 6,807 | 3,548 ± 107 |

This fixture improves warm gradients 85.7x and short inference 102.5x. It is a
small callback-heavy example, not a general model speedup. Preparation adds
about 90 µs but the first gradient already recovers that cost. Whole-process
peak RSS is 27.00 MB baseline versus 26.84 MB candidate; retained engine memory
was not isolated. The library shrinks 40,061,568 → 40,051,008 bytes, gzip
12,360,972 → 12,360,522 bytes. Compressed bytes use Python gzip with mtime zero.

The scalar RNG canary is effectively unchanged. The first AR1 batch has a
~5% short-inference slowdown signal and ~2% warm-gradient signal, with small
absolute times. A predeclared bounded follow-up uses eight same-binary A/A
pairs and eight A/B pairs, alternating block and binary order, to compare those
signals against process-to-process variation. No repeat-until-favorable rule.

The bounded AR1 follow-up is [retained here](2026-09-28-function-exits-canary.json).
Candidate/baseline paired ratios (median ± MAD) are 1.026 ± 0.032 for warm
gradients and 1.076 ± 0.041 for short inference; same-binary A/A ratios are
0.976 ± 0.025 and 0.988 ± 0.092 respectively. The small AR1 signal remains
unresolved against that variability; this is not proof of no regression.
No further timing sweep was used to select a favorable result. AR1 has no UDF
exits, and the change adds no runtime dispatch or register-format fields. The
large, independently correct callback gain justifies this coverage migration;
small-canary timing remains a limitation of the evidence.
