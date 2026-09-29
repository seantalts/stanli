# Roadmap review: findings and dispositions

Historical independent static review via `claude`, requested by the user.
Reviewed checkout: `5b913866768f351f86eed4d5a7246414530f3b2d`; CLI 2.1.283,
requested alias `fable`, resolved `claude-fable-5-1`, high effort, read-only tools.
No builds or benchmarks were delegated. Initial authentication failed; the
subsequent review completed. This digest summarizes rather than quotes the
review. The full response, input hashes and source citations are preserved in
the original version linked below; ignored local prompts/streams were under
`.cache/fable-interpreter-roadmap-review/`.

| Finding | Disposition |
| --- | --- |
| Deletion inventory omitted lowering-time interpreter use | Accepted: count preparation, folding/admission probes and initialization, not only hot evaluation. |
| Coverage could count speculative or rejected paths | Accepted: report final selected graph/register/callback/output structure and distinguish later host probing. |
| Amortization could hide startup cost | Accepted: compare preparation, first and warm work separately; a fixed gradient budget is a scenario, not user latency. |
| Code-generation linking/ownership assumptions were unproven | Require concrete deployment contracts and measured evidence. A losing bounded emitter does not disprove every future representation. This research is now tabled. |
| Exclusive timing would require intrusive instrumentation | Begin with selected structure, coarse entry evidence and existing inclusive profiles; do not add nested times or infer exclusive solver cost from a standalone callback benchmark. |
| Retention and feature expansion were mixed | Preserve already supported behavior, including bounded recursion in the standalone function API. Baseline matrix/nested shapes before claiming coverage. |
| Existing parity tests might cover RNG migration | Correction: ordinary cross-path checks exclude stochastic columns. Dedicated independent seeded output and stream-continuation tests are required. |
| Engine labels blurred machine code and derivative programs | Clarify generated adjoints, native worker graphs and per-callback interpreter construction without unsupported timing claims. |

Strict interpreter diagnostics must survive catches that normally convert
speculation failures to refusals. Runtime numerical gates were not changed by
this review. The original recommended sequence subsequently changed through
implementation and user direction; it is not a pending task list. The current
[execution roadmap](2026-09-28-execution-engines-and-roadmap.md) is authoritative
for this research thread. Fable review remains reserved for rare overarching
plans, not routine cleanup or every implementation slice.

## Full historical record

[Unabridged plan, intermediate measurements and review history](https://github.com/seantalts/stanli/blob/22cf0845bcd735e66f1e77e484a36f09657367aa/notes/2026-09-28-fable-roadmap-review.md).
Compacted on September 29, 2026; historical measurements and unresolved limits
were retained, while repeated instructions and draft implementation code were removed.
