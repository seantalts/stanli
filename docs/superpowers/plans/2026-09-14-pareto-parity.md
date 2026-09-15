# Pareto end-to-end parity

User objective: reach CmdStan/Stanli >= 1.0 for the unchanged educational
Pareto source-to-CSV benchmark. Continue the general fixes on PR #367.
Baseline commit: `4c25331f`; clean, five commits ahead of current main.
Baseline binaries and embedded compiler are in `/tmp/stanli-pareto-baseline-367/`.

Evaluator: keep all original sources/data, NUTS settings, 1000 warmup/1000
saved draws, full 17-digit file output, source compilation included, and
already-built vectorized CmdStan. Require three or more alternating pairs,
pointwise CmdStan lp/gradient/all-output parity, unrelated educational
canaries, whole CTest/corpus tests for shared changes, and adversarial
refusals. Preserve effects, exceptions and fallback; never key on source
identifiers or silently relax correctness to reach the timing target.

Fresh phase profile: 27.23 ms preparation (8.13 ms runtime lowering, including
7.29 ms generated quantities), 15.63 ms NUTS, 4.97 ms output. Instrumented
opcode accounting attributes 85% of gradient time to one 118-instruction
scalar island with 109 registers and three calls. It contains forward-only
branches and rejects, but no back edges; generated differentiation currently
refuses it, so reverse replays Stan Math autodiff.

Independent hypotheses:

- Compiler dataflow overhead: three constant-propagation invocations total
  about 12.8 ms in a diagnostic producer run. Profile the solver/partial
  evaluator before changing optimization policy. A semantic-equivalent
  implementation or avoiding demonstrably redundant work is preferable to
  dropping optimization passes.
- Acyclic scalar control: existing instruction adjoints could be enabled
  with recorded branch execution and conditional checkpoints, avoiding the
  replay. Requires path-correct reverse ordering, conditional register
  writes, sharing/alias proof, and refusal of back edges or unsupported
  active operations. Extrema in guard calculations are another current
  refusal. This is a larger beam; first quantify its available headroom.
- Architectural counterproposal: use a lighter source optimization policy
  or retain UDFs/loops rather than expanding them into expensive dataflow
  input. This could remove compiler and lowering work together but may
  lose runtime optimization. Test as an isolated ablation before assuming
  the current pass sequence is essential; do not ship slower corpus paths.

Diagnostic modifications are confined to the isolated stanc3 source checkout;
restore them before building/stamping a production compiler. The accepted candidates and current evidence are recorded below.


## Implemented candidates and proof obligations

1. **Bounded signature-resolution cache.** Memoize only immutable built-in
   lookups, keyed by name plus the complete qualified argument types. Cache
   successful, ambiguous and unsuccessful matches; never cache a mutable
   user environment. A compilation owns its table, capped at 4096 entries;
   exception-safe scope restoration also supports nested compilation. The
   first Partial_evaluator-only prototype saved roughly 6.7 ms in a matched
   five-pair baseline/candidate comparison (49.118 → 42.385 ms). Moving the
   cache to the shared immutable lookup adds little further target benefit
   but avoids duplicating the signature-matching interface.
2. **Nonthrowing speculative probes.** Runtime-valued scalars are normal
   constant-folding misses. Return an optional value instead of repeatedly
   unwinding through `cint`, `creal` and the MIR interpreter. Required geometry
   retains its existing evaluator; real-valued comparisons never truncate,
   short-circuit expressions inspect only evaluated operands, and fixed shape
   queries still specialize independently of parameter values. Runtime lowering
   fell from 8.13 to 1.32 ms; write-array lowering from 7.30 to 0.84 ms.
3. **Adjoints for forward-only control.** Reject back edges, malformed jumps,
   unsupported active operations and non-dominating conditional definitions.
   Definite initialization over the forward DAG proves that the existing
   single-writer copy aliases remain valid. Original arithmetic, calls, reads,
   exceptions and guards remain in place. Each block sets a fresh execution
   flag, cleared at the start of every evaluation. Reverse visits only executed
   blocks, in reverse order; checkpoints execute on the same forward path as
   their original instruction. Pools, calls, instructions and register counts
   commit only after all checks succeed. The proof analysis is bounded to
   4 MiB; larger/unsupported programs retain replay. Guard-only extrema have
   no derivative demand; active extrema still refuse, including a fourth
   density argument. Generated quantities skip adjoint generation entirely.
   Pareto sampling fell from roughly 15.6 to 8.8 ms. Its island changes from
   118 instructions/109 registers/replay to 129 instructions/119 registers,
   109 adjoint instructions, ten path flags, and three unchanged kernel calls.
4. **Lazy immutable overload sets.** Upstream already serializes signatures
   during its build, but deserializes every overload at first use. Serialize
   and load each name's overload list independently; preserve order and full
   type data. The environment exposes the same eager lists to its consumers
   while internally delaying unused built-ins. Undefined-function validation
   examines only source function names, in the same lexical order as the
   former full-map scan. User bindings remain eager, shadowing is preserved,
   and diagnostic/full-registry enumeration still forces the requested data.
   This avoids about 6 ms of startup work on the target without changing
   optimization policy. Producer tests verify that an ordinary compilation
   leaves most of the 571 built-in sets unmaterialized, and undefined function
   declarations still fail.

All four changes inspect semantic structure or immutable types, never source,
model or variable identifiers. The explicit Pareto name in the educational
benchmark selects its stronger 1.0 speed floor only; it does not affect the
compiler or runtime.

## Counterproposal and experiments

A diagnostic producer omitted constant/copy propagation and dead-code passes
while retaining partial evaluation and vectorization. Pareto still produced
byte-identical seeded CSV and its remaining source passes took about 0.4 ms,
versus roughly 5 ms with the normal suite. The full compiler's eager signature
initialization remained a separate cost. The lightweight policy is deferred:
removing passes globally would need a larger runtime-regression study, while
lazy signature loading removes unnecessary work without sacrificing any pass.
No diagnostic policy or altered pass sequence is shipped. Diagnostic CLI and
Optimize.ml changes in the isolated stanc3 checkout were restored before the
published native/JavaScript builds.

## Integration evidence (current)

- 247/247 Release CTests; the separately rerun educational gate unit tests
  also enforce Pareto >= 1.0 while retaining >= 0.5 for the other models.
- 254/254 reference corpus models, 976,394 values, using the unchanged
  numerical policy and existing reported ill-conditioned cases/known gap.
- 13/13 educational fixed-point, all-output and sampling smoke checks;
  worst scaled CmdStan error 8.30e-15.
- 52/52 full CSV files match the PR baseline bitwise, across 13 models and
  four seeds, 1000 warmup plus 1000 saved draws.
- Native/JavaScript portable producer byte parity in general and model-only
  modes; cached/uncached MIR equality and nested cache/error tests.
- Adjoint oracle tests cover path toggling in reused storage, nested branches,
  overwritten conditions and primals, conditional kernel calls and four-argument
  densities, skipped invalid expressions, rejects, guard-only extrema, all
  256 dyadic branch combinations, and transactional near-miss refusals.

The preliminary eleven-pair confirmation measured 26.145 ms Stanli versus
29.027 ms CmdStan (1.110x). The final fresh five-pair benchmark confirms **1.072x**:
30.746 ms Stanli (MAD 0.662 ms) / 32.974 ms CmdStan (MAD 0.445 ms).
All 13 performance/posterior rows pass. ASan+UBSan adjoint, write-array and
program-conformance checks pass, including the final four-argument density
near-miss tests. A matched five-pair revision comparison measures Pareto
49.456 → 27.632 ms (1.790x); all 13 complete medians improve, with small
canary differences treated as inconclusive. All six seeds in that comparison
also produce identical CSVs. Maximum RSS in a separate single probe falls
27,148,288 → 17,416,192 bytes. Runtime binary SHA256:
`f4134acf67005811065530e3ee6576a57bcaaedf45dcfa3d647590bc49f140a2`.

Accepted implementation commits: `1f58237f` (signature work), `0db20e6b`
(speculative probes), `e46e360f` (acyclic adjoints). The stronger Pareto floor,
raw observations and report are the final evidence commit. No implementation
work or failing correctness/performance gate remains. Raw investigation
artifacts are under `/tmp/stanli-pareto-*`; durable evidence is in
`tests/educational/pareto-benchmark-results.json`,
`pareto-matched-revision-results.json` and `pareto-revision-parity.json`.

Reproduce with the commands in `tests/educational/RESULTS.md`.
