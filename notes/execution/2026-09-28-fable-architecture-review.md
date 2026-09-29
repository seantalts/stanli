# Fable review: General value representation

**Verdict: the distinction is sound and option 2 is justified as a bounded experiment, with five corrections before it is used as a decision record.**

**What holds up.** The fixed-geometry claim is supported by the code. Every field of Range is a compile-time value, registers are never recycled, and the header states that every size and loop bound is known at compile time. The three surviving requirements are real and are already served by MirInterp today: in test_function.cpp the sized function falls back to the interpreter on one-off signatures after cache saturation, dynamic_result asserts an interpreter event, and descend relies on the interpreter for recursion, its depth guard, and recovery. Given the stated deletion goal, "an alternative general representation or loss of support" is a fair dichotomy, not an artificial one. The proposed experiment surface maps one-to-one onto those three tests, which is the right first scope.

**Corrections.**

- **Options 2 and 3 are endpoints of one continuum, not distinct forks.** A "separate compact typed-value program" is what MirInterp becomes if you pre-resolve names and callees, number its slots, and make frames explicit in place. The note should say that the experiment decides the representation and that "new engine beside MirInterp" versus "staged rewrite of MirInterp" is an implementation choice made after measurement. The rejection of option 1 is argued rather than measured, which is acceptable only because the experiment keeps hot instructions untouched.

- **The AD contract is missing and it is the axis that actually separates option 1 from option 2.** Retained callbacks with runtime-sized locals are listed as surviving fallbacks, but the experiment is value-only and defers autodiff to a later gate. Decide up front whether the value program executes under a scalar type parameter, records to the existing tape, or carries its own adjoint stream. If the answer is "same builtins with a var scalar", option 2's separateness is cheap; if it needs its own adjoints, the two options converge on the same lifetime problem the note attributes to option 1 alone.

- **Add a preparation-time probe gate.** ProgramCompiler folds through the host interpreter via the extern_int, extern_ints, and extern_real hooks, which the header says "already owns those semantics". These are many tiny evaluations at model construction. Compiling a slotted program per probe can be slower than tree-walking a small expression. Require measured probe cost against MirInterp on ordinary models, and require zero preparation cost when a model has no dynamic surface.

- **Add handoff and concurrency gates.** Specify that value-program containers use the same storage layout as Range and DataMap so crossing between the flat Program and the value program is pointer passing, not conversion. The test file exercises concurrent specialization and eviction, so per-call slot storage must be thread-safe from the first prototype.

- **Define the report contract and a success criterion.** Tests key on the execution report strings for the value engine and interpreter events, and two tests assert an interpreter event. The experiment needs a named engine label and updated canaries, or "unchanged canaries" is ambiguous. State what the prototype must show to continue: parity on the three tests plus a measured reduction in first-call or one-off-signature cost on a real fallback workload, such as the GQ runtime-real callbacks now validating.

**Unsupported statements.** None are false, but "Careful separate opcodes could avoid a branch" is the only argument offered against option 1, and it is speculation the experiment does not test. Keep it labelled as such.

## Dispositions

The decision note now treats a staged MirInterp rewrite as an implementation
route to the same representation. It fixes the AD contract to a scalar-generic
value program reusing Stan var/tape semantics, adds probe cost, concurrency,
reporting and measurable continuation gates, and preserves default canaries.
One review recommendation required correction: existing Range and DataMap
container layouts are not universally interchangeable. Zero-copy handoff is a
proof-dependent optimization; layout adapters and their cost are explicit gates.
