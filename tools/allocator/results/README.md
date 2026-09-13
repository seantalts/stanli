# Retained allocator rollout evidence

Read the [decision and limitations](../../../docs/allocator-rollout.md) and
[predeclared measurement plan](../../../docs/allocator-rollout-plan.md) before
using these ratios. Defaults remain SYSTEM / alignment OFF; the combined
candidate's local large-vector/eight-worker loss repeats in 12/12 rounds.
Linux ARM's normal 8/four-worker throughput also loses about 23% in its first
screen; Linux x86_64 loses 29–34% on three small-model/four-worker cells.
Each is negative in all five rounds; no separate confirmation was run.

| Dataset | Source / CI run | Measurement boundary |
| --- | --- | --- |
| [Local](local-2026-09-13/README.md) | `cf03f1c1`, M3 Ultra | Complete 52-cell screen, fixed confirmation, CLI ablation, memory retention, and final-source host/native checks |
| [Apple ARM first CI screen](ci-34749792182-darwin-arm64/summary.json) | [34749792182](https://github.com/seantalts/stanli/actions/runs/34749792182), `9c3d204e` | Combined private allocator / aligned loops; one/four workers |
| [Intel Mac first CI screen](ci-34749792182-darwin-x86_64/summary.json) | [34749792182](https://github.com/seantalts/stanli/actions/runs/34749792182), `9c3d204e` | Allocator only; one/four workers |
| [Windows first CI screen](ci-34750701616-windows-x86_64/summary.json) | [34750701616](https://github.com/seantalts/stanli/actions/runs/34750701616), `3c7b85fa` | Allocator only; one/four workers |
| [Linux ARM first CI screen](ci-34751295812-linux-arm64/summary.json) | [34751295812](https://github.com/seantalts/stanli/actions/runs/34751295812), `4c7e38ff` | Allocator only; one/four workers, including the new small-normal parallel loss |
| [Linux x86_64 first CI screen](ci-34751295812-linux-x86_64/summary.json) | [34751295812](https://github.com/seantalts/stanli/actions/runs/34751295812), `4c7e38ff` | Allocator only; one/four workers, including three substantial small-model parallel losses |
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

All five platforms have complete first timing screens. In
[34751295812](https://github.com/seantalts/stanli/actions/runs/34751295812), both
Mac/Linux shipping jobs, R, browser/WASM/webR, six-platform ownership, ASan and
TSan pass. Windows compiler cross-build fails during opam repository setup
(a disappearing maintenance.lock), before the Windows shipping job; the prior
completed Windows job remains separately retained. The full run is not green,
and no failed, pending or unfinished gate is counted as passing.

## Regression attribution (not replacement headline benchmarks)

The [bounded investigation](../../../docs/allocator-regression-investigation.md)
retains failed hypotheses as well as wins. Each dataset below includes every
timed/verification process, complete unique snapshots, raw stdout/stderr,
addresses, binary/source/input identities, both aliases and a SHA-256 index.
Linux host files also retain actual compiler/TLS flags and CPU topology.

| Dataset | Timed + verification processes | Question / finding |
| --- | ---: | --- |
| [Linux x86 placement](regression-34753964261-linux-x86_64/summary.json) | 600 + 30 | Moving gradient or complete test state into workers does not recover the small parallel deficit |
| [Linux ARM placement](regression-34753964261-linux-arm64/summary.json) | 600 + 30 | Original small-normal loss does not reproduce even without relocation; not a demonstrated fix |
| [Apple allocator × alignment](regression-apple-factorial/summary.json) | 160 + 8 | Large/eight-worker allocator deficit present with either alignment setting; normal 1024/four improves about 33% |
| [Linux x86 TLS](regression-34754559037-linux-x86_64/summary.json) | 300 + 15 | pthread TLS does not recover the four-worker deficit; negative substitution is not shipped |
| [Linux ARM TLS](regression-34754559037-linux-arm64/summary.json) | 300 + 15 | Both allocator TLS backends remain positive on these cells; no demonstrated reason to substitute |
| [Linux x86 startup](regression-34755149573-linux-x86_64/summary.json) | 360 + 18 | A pre-execution pause recovers the small parallel loss; limiting BLAS threads alone does not |
| [Linux ARM startup](regression-34755149573-linux-arm64/summary.json) | 360 + 18 | Same startup dependence, with wider settled-mode controls; no shipping sleep workaround |
| [Apple large-allocation fallback](regression-apple-fallback/summary.json) | 180 + 9 | Partial recovery still trails SYSTEM at eight workers; not shipped |
| [Linux x86 sustained work](regression-34755561756-linux-x86_64/summary.json) | 400 + 20 | Actual native warmup recovers the reproduced small parallel losses; standard evaluator corrected |
| [Linux ARM sustained work](regression-34755878766-linux-arm64/summary.json) | 400 + 20 | One infrastructure retry completes; reproduced small parallel losses recover with actual native warmup |
| [Apple sustained work](regression-apple-warmup/summary.json) | 240 + 12 | Large/eight-worker loss remains; full Python-source lineage deviation retained alongside fixed native identities |
| [Final evaluator verification](regression-default-warmup-verification/verification.json) | 0 + 16 | SYSTEM/default/explicit/short warmup agree on full gradients and graphs; no performance claim |

TLS datasets include user-mode CPU profiles separately from the timing
scorecard. Raw perf data and the full CI logs remain in
[34754559037](https://github.com/seantalts/stanli/actions/runs/34754559037);
profile reports, command output and complete snapshots are retained here.
Profiled elapsed times never count as uninstrumented performance observations.
Placement used an AMD EPYC 7763 x86 host; TLS used an Intel Xeon 8573C.
Their absolute timings are not pooled.

[Apple stack profiles and linked-code audit](regression-apple-profiles/code-audit.json)
retain the two sampled processes, exact full snapshots and diagnostic commands.
The previously sensitive eight-instruction normal loop remains identical,
32-byte aligned and within one 4 KiB page in SYSTEM/private/fallback binaries.
That specific loop-boundary issue does not explain this fallback's remaining
deficit. Other code/data layout effects are not ruled out by this narrow check.

The [incomplete ARM sustained-work attempt](regression-34755561756-linux-arm64-incomplete/README.md)
is retained separately, including the post-native procfs collector exception.
It is not folded into a complete timing scorecard or treated as a numerical
failure. New sustained-work measurements do not erase first-gradient or
post-load startup costs from the original results.
