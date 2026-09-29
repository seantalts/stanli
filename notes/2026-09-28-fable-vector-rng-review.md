# Fable review: remaining RNG coverage

Requested through `claude -p --model fable --effort high`, read-only tools
`Read,Grep,Glob`, no session persistence. All assistant messages report
`claude-fable-5-1`; result `is_error=false`. Raw prompt, streamed transcript
and result: `.cache/fable-vector-rng-review/`. The review ran concurrently
with validation and saw intermediate and final versions. No second review
of the dispositions below is claimed.

## Dispositions

1. **Addressed.** The implementation note documents the empty-logit crash,
   deliberate invalid_argument guard and lack of an upstream empty-case
   oracle. The test now explicitly checks the exception class. The old
   optimization text is clarified as a claim about probability-vector
   categorical RNG, and links the new logit exception.
2. **Refuted by source and regression.** `MirInterp::eval_fun` dispatches
   `Expr::Lib::UserDefined` directly to `call_udf` before calling the RNG hook.
   The O0 `gq_rng_udf_effect` fixture preserves a nested RNG argument to a UDF;
   its graph, interpreter and direct Stan Math rows/streams agree before any
   classification change. No production dispatch change was made. The new
   regression preserves this evidence. Unknown-name helper evaluation is not
   proof of double evaluation on the reachable UDF path.
3. **No new regression.** The two-dimension covariance check is present in
   the baseline 84945f1b. The square Cholesky case now shares that helper;
   refused nonsquare Cholesky shapes keep their old adapter. Existing
   multi-normal helper, graph/interpreter and dimension-refusal tests pass.
4. **Addressed.** Successful mutation rows now compare bitwise too. Contrary
   to the review's recollection, the pinned `multi_normal_cholesky_rng.hpp`
   explicitly calls `check_cholesky_factor`; rely on its source and test
   observations rather than the review's memory-based validation claim.
5. **Verified.** Zero-length cases and the runtime integer live-out index
   pass the selected-engine, complete-row and independent CmdStan tests.
6. **Addressed/limited.** Kernel/header and optimization documentation updated;
   container variant 26 is explicitly anchored, 26–28 reserved, next scalar
   additions start at 29. MIR readers' special arity lists are not exhaustive;
   graph and register handlers perform explicit arity/type checks. Expanding
   the reader's unrelated list is deferred; no weakened guard is claimed.
7. **Intentional.** The census fixtures explicitly depend on the remaining
   vectorized-gamma refusal. Migrate them again when that coverage is added.

The committed performance JSON and implementation note now provide the
public-C-API A/B, canaries and full-corpus evidence unavailable at the review's
read time. Both reference recordings were produced by the pinned CmdStan
`verify_sample.build_ref` driver, with executable/source/dependency identity
and raw outputs retained under `.cache/vector-rng-oracle/` and
`.cache/vector-rng-udf-oracle/`. Earlier array-output fixture recordings were
replaced by a fresh recording after changing the source; they were not edited
to match Stanli results.

## Original review

Review complete. The tree changed while I was reading it: the test, both fixtures and the reference JSON differ between my first and second reads. Findings below are against the later versions, with scalar `choices`/`counts`/`samples` in the complete fixture and the empty-vector guard in `vector_integer_rng_draw`. Without a diff I cannot always separate new lines from pre-existing ones. Where that matters I say so.

## Findings, most severe first

1. **Medium, contract drift (actionable).** `runtime/src/wa_interp.cpp:137-142` adds a nonzero-size check before upstream `categorical_logit_rng`. The guard is correct, since upstream reads the first cumulative-sum entry unconditionally. But it contradicts the documented contract that Stan Math is the single definition of empty-vector validation (`runtime/src/OPTIMIZATIONS.md:549-552`), and the plan note does not record the deviation. The test's own upstream leg at `tests/test_vector_rng.cpp:41-42` applies the same guard, so for family 1 with N=0 all three legs test Stanli's decision, not an oracle. Record the deviation in the note and assert the expected exception class explicitly for that case.

2. **Medium, effect preservation (present in current source; whether new is unverified).** `runtime/src/wa_interp.cpp:358-359` evaluates every argument before the name is classified, then returns false at line 423 for a `_rng`-suffixed user function such as the fixture's `selected_rng`. The UDF path evaluates the arguments again. With pure arguments this is only wasted work. A `_rng` UDF whose argument contains an RNG call would consume the stream twice on the interpreter route and diverge from CmdStan. Classify before evaluating.

3. **Medium, verify existing path.** The interpreter's non-Cholesky `multi_normal` branch now throws when the covariance entry lacks two dims (`runtime/src/wa_interp.cpp:374-377`), while the Cholesky branch silently falls to the legacy K-by-K read. `runtime/include/stanli/mir_interp.hpp:684` suggests matrix values do not always carry dims. If the previous code tolerated missing dims, interpreted `multi_normal_rng` regresses. Please confirm against the diff.

4. **Low, test gap.** In the mutation loop (`tests/test_vector_rng.cpp:170-199`) rows are compared only on recovery. When no leg throws, which is every L mutation for families 0 to 2 and, since upstream does not validate the factor, for family 3 as well, the NaN and negative-diagonal outputs are never byte-compared. Compare rows on the no-throw path too.

5. **Low, likely failure points if the runs go red (not defects).** `lower_multi_normal_rng` has no positive-length gate, unlike dirichlet at `runtime/src/lower_funapp.cpp:579`, and the test demands no fallback at N=0. Separately, `indexed = probabilities[choices]` in `tests/fixtures/gq_vector_rng_complete.stan:22` indexes with a register-region live-out int, while the only dynamic-index evidence uses a graph-produced int. Either would show as `INTERP` in `vector_rng_reference`.

6. **Low, documentation drift.** `runtime/src/OPTIMIZATIONS.md:616` and `:1458` still say `multi_normal_cholesky_rng` selects WaInterp. The `runtime/kernels/rng.cpp:3-9` header omits the three new variants. `runtime/include/stanli/rng_family.hpp:55-61` places the new variants at `NegBinomial2Log + 1`, so `ScalarRng` can no longer grow without renumbering, contrary to the "append" comment. `runtime/src/mir_reader.cpp:89` validates arity for `categorical_rng` only.

7. **Low, soft dependency.** `tests/fixtures/execution_rng.stan` and its reject twin now prove fallback via `gamma_rng(vector, real)`, which stays interpreted only because of the deliberate refusal at `runtime/src/lower_funapp.cpp:656-657`.

## Scope exclusions, not defects

Rectangular or dims-less Cholesky factors keep legacy interpreter semantics. Array-of-vector overloads stay interpreted. Upstream does not validate lower-triangularity, so the compiled path faithfully reproduces that. I could not read the pinned Stan Math sources from this tree, so upstream-behavior statements are from memory of that version.

## Validation status versus the plan

The three-way differential test, the register-region coverage, chains 0 and 3, invalid inputs, rejection and recovery match what the note promised. The CmdStan reference was re-recorded mid-review. Its shared columns match the earlier recording, which is consistent with either an independent CmdStan run or a copy, so its provenance through `verify_sample.build_ref` should be confirmed. Nothing in the worktree evidences the promised public-C-API A/B with RNG and AR1 canaries or the full-corpus run. The benchmark mode in the new test is a graph-versus-interpreter microbench, not that comparison.

## Verdict

The implementation is a sound bounded continuation of roadmap item 1. Shared owning-Eigen helpers, one effectful opcode, kernel reuse from register regions and the three-way seeded differential follow the scalar slice's contract, and I see no high-severity defect in the current sources. Findings 1 through 3 should land with the slice or be deferred explicitly in the note. Integration remains gated on the in-progress test runs and the missing perf and corpus evidence.
