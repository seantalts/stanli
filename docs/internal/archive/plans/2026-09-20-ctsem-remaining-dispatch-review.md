# Fable 5.1 data-branch delta review

Reviewed base: `680a4763b256764476bca72b7916ad312c910701 plus direct-forward checkpoint 22c2d86b`. Requested model: `claude-fable-5-1`. Read-only review via Claude CLI; no implementation or benchmark runs.

No actionable bugs found in the DataBranchFresh/DataBranch fusion. The fused path is equivalent to DataKernel followed by Branch/WhileTest on every axis you asked about.

**Why the equivalence holds**

- **Same value is read.** The retained branch reads `value(n.condition)`, which resolves to the version bound at publication, whose value pointer is the site workspace (runtime/src/structured_loop.cpp:3305). The fused path reads `context.out.data[0]`, which compile_data set to that same workspace address (runtime/src/structured_recording.inc:213). Frame sealing relocates only arena ranges (runtime/src/structured_frames.inc:265), so workspace pointers survive remapping and both reads stay the same memory on every trip.
- **Guard skip is equivalent.** The original Branch logs a guard only when the condition version is not constant. Publication sets the flag, and seal_frame copies the whole version struct and rebinds `bindings` and `node_version` through one remap (runtime/src/structured_frames.inc:334-349). So the original never logs a guard after publication, and the fused path drops nothing.
- **First-use publication lifetime matches.** Execution is constructed per evaluation in the same function that creates the transient versions (runtime/src/structured_loop.cpp:3276). The Fresh kind and the DataKernel jump flag therefore reset together. A throw inside the kernel leaves the kind at Fresh, exactly as it leaves the jump flag at zero, so the next successful run republishes.
- **Direct jumps are safe.** Only two entry points land on a retained i+1: the then-arm's done Jump when the producer ends the else-arm, and a `continue` inside a while condition, which is patched to the WhileTest. Both hit the unmodified instruction and read via bindings as before. Fallthrough from i to i+1 is always real control flow in this lowering, so fusion never creates a new path.
- **Effects and order.** One kernel execution and one effect increment in both paths. The branch contributes no effect in either. The taken target is i+2 in both, and the not-taken target is the copied branch jump, which is never mutated after lowering.

**Test gaps** in tests/test_structured_loop.cpp, none blocking

- data_recording_tests mode 0 asserts only that one branch fused. No test enters a retained Branch by direct jump while its producer is fused. A shape that would cover it: producer as the last statement of an else-arm, then a sibling If on that slot, with the then-arm taken on some trips.
- No test fuses a WhileTest. A while whose condition block ends in a proven data producer, with a `continue` inside the condition, would cover the direct-jump path into the retained WhileTest.
- The differential memcmp checks values and gradients but not guard-stream or effect-count equality between fused and unfused runs. A diagnostic assertion that the fused candidate logs zero guards on that branch would pin the guard-skip equivalence.

## Reconciliation after review

`data_branch_entry_tests` now covers both independent-entry cases, including
the sibling Branch before its producer's first publication, later frame trips,
changed imported data, and a retained WhileTest entered by a condition-block
continue. It asserts actual fusion and expected target counts, then compares
values and gradients bitwise with unspecialized recording and tree evaluation.
The focused structured-loop suite passes. No production code changed after
this review or the final performance measurements.

Separate guard/effect counters were not added. The existing ordered-control
and guard/retry fixtures remain in the passing suite; the equivalence argument
above checks the unchanged effect increment and constant-condition guard skip.
