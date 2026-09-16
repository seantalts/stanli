# COM-Poisson sampling follow-up

**All four seeds complete under the unchanged 3× CmdStan cap.** The median
complete-process ratio is **0.591× CmdStan/Stanli** (higher means faster Stanli).
This clears the cap; it remains below the separate 0.8× performance target.
These measurements supplement the frozen 199-model sweep.

| Seed | CmdStan/Stanli sampling speedup |
| --- | ---: |
| 1 | 0.659× |
| 2 | 0.535× |
| 3 | 0.582× |
| 4 | 0.576× |

## What was slow

The generated Stan function declares a 10,000-element workspace before choosing
between a series calculation and an approximation. Stanli initialized the whole
workspace even on the approximation branch, which never reads it. Forward
execution and gradient replay both paid for those writes.

The compiler now delays a constant range initialization until the branch that
needs it. It proves that initialization still precedes every read and write,
including indexed accesses, and refuses moves into or out of a loop. The model,
arithmetic, sampler, stopping criteria, and numerical tolerances are unchanged.

Of 20,000 observation evaluations reconstructed from seed 1's retained draws,
99.9% use the approximation branch. This is not a count of warmup or leapfrog
proposals. The original fixed-point gradient benchmark uses the series branch,
which explains why it was a poor guide to this sampling bottleneck. Constant
hoisting and activity-aware replay were tested separately and set aside after
failing to improve complete-process performance enough.

## Numerics

| Evaluation points | Maximum absolute log-density difference | Maximum absolute gradient difference |
| --- | ---: | ---: |
| Three recorded corpus points | 2.84e-14 | 2.13e-14 |
| 28 posterior and branch-boundary points | 4.97e-13 | 3.98e-13 |

Differences are against independent CmdStan evaluations. The additional points
have **identical density and gradient values before and after** the optimization;
outputs are identical to CmdStan at the three original corpus points. The
completed before/after seed also has byte-identical draw CSVs. Other baseline
seeds hit the cap, so no complete draw comparison is claimed for them.

[Original-point values and ULP differences](final-numerics.json) and
[additional-point values and ULP differences](extra-point-numerics.json) are
retained. [Sampling diagnostics](diagnostics.json) include all four chains for
both engines; neither has divergences or maximum-depth hits in these runs.

## Protocol and evidence

Apple M3 Ultra, macOS ARM64, Release build; four single-chain runs with 1,000
warmup and 1,000 retained draws, seeds 1–4. Each executable receives one no-argument
launch before measurements. Sampling still includes the whole CLI process and
Stanli source compilation; CmdStan model compilation is excluded. This is
warm-executable, fixed-budget runtime, not first-install time or time to equal
inferential accuracy. The cap is min(3 × the matching CmdStan duration, 900 s).
Runs are serial with all numerical thread limits set to one.

- [Sampling summary](sampling-summary.json), including per-seed times and caps.
- [All sampling experiments](sampling-experiments.json.gz), including rejected
  prototypes, commands, exact patches, input hashes, and executable identities.
- [Draws and command logs](sampling-raw.tar.gz) from the final experiment.
- [Gradient canaries](gradient-canaries.json.gz), including raw trials and identity.
- [Runtime tests](runtime-tests.txt): 253 passed.
- [Corpus replay](numerical-replay.txt): 316 existing-policy checks passed;
  1,008,755 values, unchanged numerical and domain policies.
- [R acceptance](r-ecosystem-tests.txt): 456 expectations, no failures, warnings,
  or skips.
- [Provenance and reproduction](provenance.json).

A deadline-helper process-exit race raised a PermissionError for the previous
runtime's seed 3. That baseline remains recorded as timed out. It exited and was
reaped before the candidate started; all candidate and reference timers completed
normally. The full log and original record are retained.
