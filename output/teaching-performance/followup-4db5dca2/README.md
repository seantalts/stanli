# Separate GP and sampling follow-up

These checks test the runtime changes integrated in `4db5dca2`. They are **not
rows added to the complete `fd5e0ecacddc7047` sweep**. The measured checkout was
`6e462c2e` plus the retained candidate patch; `source-equivalence.json` verifies
that its two changed runtime files exactly match `4db5dca2`. `identity.json`
records the executable hash, patch hash, inputs' source run and settings.

Fixed-location exp-quad covariance gradients now retain the same traversal and
reduction order as CmdStan's specialized callback. All three previously failing
GP fixtures match every recorded value at all three reference points exactly
on this machine. Including the Rethinking m14.8 canary, the replay compares 900
values; the largest scaled discrepancy is 3.00e-16. Existing cross-platform
ill-conditioning policies remain unchanged pending broader evidence.

Four independent seeds, each with 1,000 warmup and 1,000 retained draws, use the
same frozen model/data bytes and the original min(3× CmdStan time, 900 s) cap.
The table measures complete CLI elapsed time, with Stanli preparation included
and CmdStan compilation excluded. All seeds completed.

| Fixture | Stanli median seconds | CmdStan median seconds | CmdStan/Stanli CLI ratio | Divergences Stanli / CmdStan | Diagnostic screen |
| --- | ---: | ---: | ---: | ---: | --- |
| `sw_gp` | 1.033 | 1.233 | 1.194 | 16 / 283 | review in both |
| `i320_gp_expquad` | 0.623 | 0.860 | 1.381 | 28 / 26 | review in both |
| `s2_gp_by_gr` | 0.545 | 0.814 | 1.492 | 2 / 4 | review in both |
| `s2_invgaussian` | 0.0248 | 0.0495 | 1.993 | 1689 / 1671 | review in both |

Inverse Gaussian samples in both engines; the original fixed benchmark points
are outside its domain in both. Sampling completion does not supply the missing
finite-point numerical comparison. All four fixtures still have diagnostic
flags, so these elapsed times do not establish time to reliable inference.
COM-Poisson remains a separate dynamic-loop support gap.

## Retained checks

- `sampling-results.json`: all per-seed durations, caps, input and reference executable hashes.
- `sampling-diagnostics.json`: divergences, depth, R-hat, ESS and E-BFMI.
- `gp-numerical-replay.txt`: three-point GP checks and the unrelated m14.8 canary.
- `runtime-tests.txt`, `r-ecosystem-tests.txt`, `numerical-replay-all.txt`: 249 runtime tests, 456 R expectations without warnings/failures/skips, and 316 existing-policy reference checks. Exceptions and domain refusals remain part of the latter policy.
- `ci-validation.json`: [PR CI at b5b61a80](https://github.com/seantalts/stanli/actions/runs/35082758748), including Linux runtime R acceptance and compiler comparisons. This is the PR scope, not the full macOS/Windows/sanitizer matrix.

Raw per-seed CSVs and command logs remain separately retained locally under
`/tmp/stanli-teaching-perf/blocker-sampling-v7/`. They are not part of the full
sweep's raw archive. No model inputs, sampler settings, RNG or numerical gates
were adjusted to produce these results.
