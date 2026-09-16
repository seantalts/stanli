**Summary.** No blocker in the code as written. The proof is valid for the instruction set it models, the state-owned workspace closes the TLS problem, and the dead-constant pass is safe. The unverified point that matters most is whether the real COM-Poisson island actually passes the proof, since the measured ratios all came from the ungated TLS prototype.

## Proof review

**Valid, with a scope caveat.** `program_initializes_reads` at `runtime/src/program.cpp:603-695` is a correct must-write analysis. Initial state is all-ones, joins intersect, entry is seeded from live-ins and also intersected with back-edge predecessors into instruction zero, reads are checked against the incoming set before the instruction's own writes, out registers are checked at the exit node, and unreachable blocks count as uninitialized. Treating CALL as writing only its output matches `run_call_var`, which keeps scratch in a private arena buffer. I checked the interpreter's actual reads for DYN_SET, DYN_INDEX, DYN_LSE_RANGE, RANGE broadcast, DENSITY with arity above three, and the JZ operand against `each_read`. All spans match.

The caveat is that the proof is about `each_read` and `each_write` as a model of the interpreter, not about the interpreter itself. Any future opcode whose var path touches cells outside its declared span breaks it silently. The allow-list at `program.cpp:630-636` is what guards this, since it refuses every unmodelled opcode. Say so in a comment there.

**Lifetimes are clean.** Ownership transfer by swap handles same-op reentrancy. Distinct ops own distinct states. If `resize` throws after the swap, the state is left empty and nothing dangles. Executor copy rebinds fresh states. Pool leases move executors between threads, but stale handles are only ever copied, never dereferenced. Treat my earlier leaked-TLS suggestion as superseded.

**Dead-constant pass is safe.** A path entering at a label between the CONST and its killing write never observed the constant and proceeds linearly to the kill with no reads, because branches are barriers. Single-register cells make any overlap a full overwrite, and the read check runs before the write check so read-modify-write retains the constant.

## Findings

- **High priority, unverified: the fast path may not be admitted on the real model.** The final gated version has never been shown to take the fast path on a lowered region with a back edge. An unreachable instruction after a JMP or a read initialized via a path the analysis cannot see refuses silently, and you would benchmark the fallback. Before timing anything, add a bind-time trace line to the existing prep counters reporting replay islands proved versus refused. Also add a test on an existing while-lpmf fixture asserting the factory returns non-null for its island.

- **Medium: proof cost is paid per executor bind and per island.** It runs again on every executor copy and on every `higher_order_eval` invoke during constant folding. The result depends only on the program. Compute it once in `runtime/src/lower_stmt.cpp` after the native-adjoint decision, store a flag on `IslandProg`, and have `island_state` read the flag. Today the transient bound is about 8MB and a few milliseconds per island. Thousands of islands across pool leases becomes seconds at bind.

- **Low: undocumented environment switches.** `STANLI_NO_REPLAY_REUSE` and `STANLI_NO_DEAD_CONSTANTS` appear only in source. Add them wherever the other switches are listed.

## Tests still needed

1. Fallback actually executed. Run a refused program through an executor so `island_bwd` sees a null state, and compare gradients with the accepted variant. Today the refused case only checks the factory.
2. Equality between the reuse switch off and default on a lowered fixture with a back edge, not only hand-built programs.
3. Executor copy taken after a gradient has populated the source state, then both evaluated.
4. Dead constants with a branch target between the CONST and its kill, pinning the reasoning above.
5. Corpus-wide sentinel run. Pre-fill workspaces with a var pointing at one heap vari and assert its adjoint is zero after every gradient. This is the only check that catches a declared span narrower than the interpreter's real reads.

## Next step

The arena-vector change to `binary_bwd_typed` at `runtime/kernels/scalar_binary.cpp:42-77` is the right next lever. The three vectors are consumed inside the nested scope, so arena storage is recovered at scope end, and vari creation order is unchanged, so the bitwise gate should hold. Prefer it over closed-form derivatives.

Decision: proof valid, no code blocker, cache the proof at lowering, and do not benchmark until the admission trace confirms the COM-Poisson island is proved.