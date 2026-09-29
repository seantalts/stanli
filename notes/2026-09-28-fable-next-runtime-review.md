# Fable review: keep working in roadmap order

Read-only `claude --model fable --effort medium` review.
Accepted: the native priority already selects the next step; do not manufacture
a blocking approval at this checkpoint. Updated the decision note accordingly.

**Verdict.** The note names a real fork, but it is not a new one and it does not need a blocking approval. Option 1 is the roadmap's existing order (milestones 2 and 3 before 4); option 2 is a reordering to fund milestone 4 early. Given your stated priority, the recommendation follows directly, so work should continue on option 1 now with option 2 re-evaluated after the nested-tape profile. The note should say that instead of framing it as a user decision.

**Is the recommendation supported while preserving interpreter deletion?** Mostly yes. Option 1 keeps fallbacks, keeps MIR entry points tested, and the roadmap already states deletion is the last step of route replacement, not a forcing function. Two things weaken the support:

- **The standalone numbers are not model-throughput evidence.** Warm calls improved roughly 9x to 11x, but those are C ABI standalone function calls. The note's "advances measured native throughput" reads as if model gradient gains were shown. They were not. Option 1's payoff still rests on the unprofiled nested-tape cost.
- **Option 1 has no exit gate.** "Profile real models" needs which corpus sources, a target improvement, and the AGENTS.md no-regression check on small and medium models. Milestone 3 in the roadmap already specifies this; the note should cite it rather than restate a vaguer version.

**Incorrect or unstated claims.**

- **"First fork after the completed slices."** The roadmap already sequenced these milestones, and the stencil-JIT and dispatch research were tabled by you. The note should describe this as "continue the roadmap order versus reorder to milestone 4."
- **Cache admission is described as settled.** The plan section calls the two-miss policy a "bounded next experiment." The performance summary does show it winning on the rotating workload, so the claim holds, but the note should cite that measurement and the initial-candidate regression it corrected.
- **First-call regression is omitted.** The branch-exit fixture's first call went from about 157 to 195 µs. Small in absolute terms, but it touches the time-to-first-posterior priority and should be recorded rather than hidden by warm numbers.

**Concrete missing gates for eventual deletion.**

- The milestone 0 census manifest of mandatory interpreter uses is not referenced. It is the only way to measure whether option 1 is shrinking the deletion surface or just adding compiled coverage beside it.
- The integer-result contract is absent. The plan defers integer standalone results to the interpreter until 32-bit mirrors and overflow behavior are specified. That is a named deletion gate and belongs in the note.
- Option 2's experiment lists what to measure but no budgets: no binary-size ceiling, no preparation-time ceiling, and no browser or Wasm mode requirement, though the roadmap says non-native modes need an installation strategy before deletion.
- "Public standalone API already exposes some of these behaviors" should enumerate them with fixtures: runtime-sized returns, bounded recursion, changing local extents, integer results. Without the list, "preserve them" cannot be checked.

**Recommended edit.** Replace the closing "user decision" paragraph with: continue option 1 per roadmap milestones 2 and 3; first deliverable is the nested-tape profile with a stated target and no-regression gate; revisit option 2 only if the census shows the remaining mandatory interpreter routes are the bottleneck for a user-visible workload. Add the integer contract, census reference, and size and prep budgets as explicit gates.
