# Retained allocator rollout evidence

Read the [decision and limitations](../../../docs/allocator-rollout.md) and
[predeclared measurement plan](../../../docs/allocator-rollout-plan.md) before
using these ratios. Defaults remain SYSTEM / alignment OFF; the combined
candidate's local large-vector/eight-worker loss repeats in 12/12 rounds.

| Dataset | Source / CI run | Measurement boundary |
| --- | --- | --- |
| [Local](local-2026-09-13/README.md) | `cf03f1c1`, M3 Ultra | Complete 52-cell screen, fixed confirmation, CLI ablation, memory retention, and final-source host/native checks |
| [Apple ARM first CI screen](ci-34749792182-darwin-arm64/summary.json) | [34749792182](https://github.com/seantalts/stanli/actions/runs/34749792182), `9c3d204e` | Combined private allocator / aligned loops; one/four workers |
| [Intel Mac first CI screen](ci-34749792182-darwin-x86_64/summary.json) | [34749792182](https://github.com/seantalts/stanli/actions/runs/34749792182), `9c3d204e` | Allocator only; one/four workers |
| [Windows first CI screen](ci-34750701616-windows-x86_64/summary.json) | [34750701616](https://github.com/seantalts/stanli/actions/runs/34750701616), `3c7b85fa` | Allocator only; one/four workers |
| [Apple ARM validation rerun](ci-34750701616-darwin-arm64/summary.json) | [34750701616](https://github.com/seantalts/stanli/actions/runs/34750701616), `3c7b85fa` | Combined candidate at formatted source; retained separately, including substantial A/A shifts and negative signals |

Each complete CI dataset includes 92 verification and 480 timing processes,
all 12 timed cells and both identical-binary controls, full unique snapshots,
model/data/MIR identities, source/binary hashes, build cache, and exact commands.
The paired ratios and ranges are not confidence intervals or whole-inference
speedups. Source and binary identities belong to each dataset, not necessarily
the latest documentation commit.

The first run's Linux/Windows measurement target failed to build; the second
run's Linux metadata read failed before timing. These are not measurements.
The second run was canceled after its Windows and Apple ARM jobs completed
to unblock the corrected run; its unfinished Intel Mac screen is not included.
The artifacts in the linked runs retain those failures and partial outputs.

Linux measurements and remaining release checks are still pending in
[34751295812](https://github.com/seantalts/stanli/actions/runs/34751295812).
No pending or unfinished run counts as a passing performance gate.
