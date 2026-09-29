# Compile value-only callbacks with runtime real arguments

Base52ee7217. Native performance remains the priority; JIT research remains
tabled. The matrix context fixture establishes a separate existing gap:
generated quantities are interpreted because a runtime real argument carries a
data-only annotation and retained-call packing insists on folding it.

The proof is phase-specific. In write_array, real values can depend on the draw
while their C++ scalar type is double. Route real callback actuals through the
existing runtime theta buffer in that phase, preserving source order, shape
and evaluation count. Keep compile-time integer specialization and solver
controls unchanged. Separate the count of runtime reals from the solver's
activity bits: write_array has no active scalar arguments and must choose the
double/double solver path. Outside this phase retain current classification.
Do not merely turn every runtime argument into a var; that would alter solver
work, memory and potentially adaptive numerical results.

Small implementation: a value-only runtime-real packing policy for the shared
program/quadrature packer and modern graph ODE packing. Existing RhsArg
is_param then denotes which buffer supplies a real argument, not its scalar AD
type; solver activity is already carried separately. All five retained solver
families must use the phase-correct zero activity mask. No new public ABI,
value representation, JIT or numerical algorithm is needed.

Evaluator before edits: keep the existing independent CmdStan context/edge
references unchanged. Require graph-backed GQ for the ordinary context model,
compiled nested callbacks for all five families, and double solver activity.
The adversarial dynamic-local callbacks must retain interpretation locally
without forcing the entire GQ block into interpretation. Compare full output
rows at all reference points, including changing branch values and runtime
matrix values. Add runtime scalar/vector data-qualified actuals, integer and
control refusals, and ordinary log-probability canaries as needed. Keep the
interpreter itself available: runtime-shaped GQ outside this proof still needs
it. Retain a deliberately whole-block-interpreted fixture to test value-only
higher_order_eval geometry after the original context fixture moves up.

Measure existing MIR preparation, first/warm output rows, first/warm gradients,
complete inference plus outputs, memory and library size against52ee7217.
Use the combined solver fixture for correctness; isolate an ordinary matrix
ODE GQ for useful performance measurements if the combined tiny solves obscure
costs. Require independent output fidelity, unchanged native model gradients,
and a measured output-path improvement before adopting. Review this proof and
evaluator with Fable before implementation.

## Bounded canary follow-up, declared before results

Six fresh-process phase pairs show the ordinary branch-ODE canary's short
inference+outputs median increasing from3.164ms to3.387ms (+7.1%), although
warm gradient and output-row differences are small. Do not silently dismiss it.
Run exactly eight A/A and eight A/B fresh-process pairs with repeated warmed
100-warmup+100-draw inference plus output rows in200/250ms windows. Alternate
order; compare paired ratios/MAD and require identical draw/output hashes.
Keep the original result. Overlap with A/A variation is inconclusive, not a
proof of no regression. A resolved A/B slowdown outside that variation needs
investigation before saving the implementation. No open-ended timing retries.
