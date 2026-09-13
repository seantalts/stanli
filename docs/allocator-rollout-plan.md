# Private allocator default rollout: evidence gates

2026-09-13. User authorized remaining measurements, benchmark documentation,
a focused current-main PR, and auto-merge only after validation. Original
research worktree and its divergent commits are preserved. PR baseline:
7bbda72c008c16cf227c4db84b0d7f95a3ffc65e.

## Candidate and stopping rules

Compare current-main SYSTEM / alignment OFF with private mimalloc / alignment
AUTO (32-byte loops on supported macOS arm64 Apple Clang, unchanged elsewhere).
Initially retain SYSTEM/OFF defaults until evidence supports promotion. The
private allocator affects native shared libraries, not process-wide C++
new/delete, Python/R heaps, static/CLI allocation, or WASM/webR.

Promotion requires:

1. Full log-density/gradient bytes agree with the matched current-main
   baseline at eight changing points, before/after warm replay and across
   independent worker tapes. No numerical tolerance, validation removal or
   model-name dispatch. Exercise source compilation, exceptions and host
   ownership through shipping Python/R interfaces too.
2. Current-main warm-gradient screen: all 23 frozen fixtures at 1/4 workers,
   plus normal 8/1024/262144, gamma 16384, eight schools and GP at 8 workers.
   Five balanced rounds, two processes/slot/round, three blocks/process,
   SYSTEM/candidate and an identical alias for each. Per-cell baseline
   calibration targets 150 ms per block and is then frozen. Report medians,
   round ranges, all paired signs, full snapshots and process peak RSS.
   Independent changing-point and host-lifetime checks precede timing.
3. Treat a repeatable unexplained >=3% throughput loss versus the actual
   baseline as a stop/diagnose signal, not a regression budget. Report smaller
   consistent negatives too. One bounded confirmation of flagged cases plus
   positive/unrelated controls is allowed; no rerunning until a favorable
   sign. No geomean may conceal a losing case.
4. Native private-ownership execution on macOS arm64/x86_64, Linux
   arm64/x86_64 (GCC plus a Clang check), and Windows MinGW/UCRT x86_64.
   Cross-linking does not satisfy this gate. Full shipping wheel/runtime
   jobs and available host suites must validate the candidate before rollout.
   Retain SYSTEM fallback for unsupported/sanitized/LTO configurations.
5. Repeated-gradient retention and changing model-size/lifetime checks;
   investigate unexplained retained-memory growth, not just speed.
6. Full local native/Python/R tests, configuration defaults and mechanical
   rollback checks, final linked-loop inspection on Apple ARM, export/ownership
   checks, and normal CI. Keep alignment platform-scoped, not guessed for x86.

## CI and publication

GitHub auto-merge is enabled for this repository, but the current required
check only covers the representative manylinux workflow. Make the focused
native allocator matrix a dependency of that existing required gate. Do not
bypass it, weaken branch protection, or enable auto-merge with known failures.
Full release-platform validation can use manual branch runs before readiness.

The published CmdStan table uses a static CLI evaluator. Add a separate
shared-library before/after table with its own boundary/provenance. Do not
multiply old CmdStan or source-to-CSV numbers by these allocator speedups.
If common loop flags affect the CLI, measure that consumer separately and
either refresh its measured columns honestly or retain a scope that leaves
the existing table applicable. Do not relabel historical timings as fresh.

The portable benchmark DSO is separate and non-installed; it reuses production
objects and the same private allocator function, with an additive measurement
entry. Its native loop is derived from the prior evaluator; cross-platform
memory queries are outside timed regions. Prove the refactor against the
retained host evaluator before using its numbers. No remote run is claimed
until its actual execution result is available.
