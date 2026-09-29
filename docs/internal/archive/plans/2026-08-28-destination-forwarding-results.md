# Generic destination forwarding: implementation and results

**Date:** 2026-08-28

**Status:** PR candidate; implemented, rebased, performance-atomically
squashed, validated locally, and ready for review; not merged; high-effort
Fable review is a conditional GO with all local conditions satisfied;
cross-platform CI remains a pre-merge gate

**Branch:** `codex/destination-forwarding`

**Atomic implementation commit:**
`394e973a572e9821ac61f3b25e5851d760367384`

**Full-corpus measured runtime commit:**
`46e6b158310afc043b939bcaf388ba7e239288a8`. The final runtime source differs
only by clang-format line wrapping, and its rebuilt `bench_grad` executable is
byte-identical.

## Decision

Destination forwarding is worth preparing as a small, independently shippable
feature. It is not a JIT and it contains no model-specific code. It is a
model-blind register-program optimization that removes an adjacent temporary
copy by making the producer write the copy's destination directly.

On the final activation-preserving candidate it makes IOHMM 22.9% faster,
two accelerator examples about 15% faster, two other HMMs 5-7% faster, and a
third HMM 1.9% faster. The full timed corpus is about 0.8% faster
geometrically. All 119 timed corpus models clear the 3% per-model regression
policy after five noisy screen cases are rerun. The pass adds 211 production
lines and removes 22; the remaining 298 added lines are safety and regression
tests. This is a materially better payoff/complexity ratio than the dormant
native-JIT prototype.

The PR branch is based on current `origin/main`. Its implementation, activation
gate, and tests are one performance-atomic commit; this report is separate. The
intermediate research commit that exposed the ungated optimization remains
only on the unpushed research branch and is not part of the proposed history.

## What it does

The program compiler commonly emits this form:

```text
producer temporary <- inputs
MOV[/R] destination <- temporary
```

The pass rewrites it to:

```text
producer destination <- inputs
```

and deletes the copy. This works for scalar and ranged outputs and reduces the
forward program, generated adjoint, and register file together.

The legality checks are deliberately conservative:

- the producer and copy must be adjacent;
- branch-bearing programs are left unchanged;
- the copy must consume exactly the producer's full output span, and its
  temporary and destination spans may not overlap;
- `MOV`, `MOVR`, `CALL`, no-output instructions, and instructions without a
  generated adjoint are not treated as producers;
- the temporary must be written once, read once by the copy, and not be a
  seeded or live-out register;
- the destination may not overlap a producer input, so an out-of-place
  operation is never silently made in-place;
- when a reverse rule saves the producer output, the destination may not be
  overwritten later; and
- after rewriting, the existing compactor and adjoint generator remain the
  authorities for register mapping and reverse-code validity.

`STANLI_NO_PROGRAM_DEST_FORWARD=1` is the A/B and emergency opt-out.

## Activation and cost-model design

The first implementation let its own instruction savings affect island
admission. That activated a marginal island in `brmsmono`; the newly selected
island measured 4.04% slower than the graph. The final design does not permit
that feedback loop:

1. Compile the raw candidate program.
2. Compact and generate its adjoint with destination forwarding disabled.
3. Make CALL eligibility and cost-model decisions from that established form.
4. Only after the region is accepted, compact a retained raw copy with
   destination forwarding enabled.
5. Use the optimized copy only if adjoint generation still succeeds; otherwise
   retain the priced program. Generator-refused replay-only regions are not
   changed.

A cheap syntax preflight avoids retaining a deep copy when the raw program has
no adjacent producer/copy candidate. The old `compact_program` and
`compact_island` entry points and symbols are preserved; explicitly gated
helpers have distinct names, so source uses such as `auto f = &compact_program`
remain unambiguous.

This makes the activation rule exact rather than heuristic. A 147-entry
PosteriorDB audit (120 unique models) compared feature-on with the opt-out and
found identical:

- graph op and slot counts;
- island counts and exact graph positions;
- every pre-forward cost-model diagnostic; and
- every generated-adjoint refusal diagnostic.

There were zero mismatches, timeouts, missing inputs, or stanc failures.
`brmsmono` now remains a 43-op graph under both arms. Its 20-round runtime ratio
is 1.0027, 95% CI [0.9973, 1.0063], replacing the former 1.0404 regression.

Two non-island callers use the ordinary one-pass entry point: directly lowered
runtime-control programs and ODE RHS programs. They do not need the island
price/fallback protocol because the rewrite forbids every destination/input
overlap (stricter than the adjoint generator's own overlap rule) and only
removes a copy, so it cannot newly make adjoint generation fail. The semantic
corpus replay corroborates output correctness for these paths; the activation
audit's adjoint diagnostics apply only to regions considered by the island
carver.

## Performance results

All ratios below are candidate / `STANLI_NO_PROGRAM_DEST_FORWARD=1`, so lower
is better. Both arms use the same executable. Decision runs use fresh processes
in alternating ABBA/BAAB order and report a stratified bootstrap interval over
paired log ratios. All speed measurements were made on one macOS arm64 host;
cross-platform performance remains unmeasured, while normal cross-platform
correctness CI is a pre-merge gate.

### Focused decision runs

| Model | Candidate / baseline | 95% CI | Improvement |
| --- | ---: | ---: | ---: |
| `iohmm_reg` | 0.7714 | [0.7681, 0.7743] | 22.9% |
| `accel_gp` | 0.8439 | [0.8402, 0.8483] | 15.6% |
| `accel_splines` | 0.8520 | [0.8509, 0.8538] | 14.8% |
| `hmm_drive_0` | 0.9345 | [0.9257, 0.9428] | 6.5% |
| `hmm_gaussian` | 0.9419 | [0.9343, 0.9538] | 5.8% |
| `soil_incubation` | 0.9551 | [0.9517, 0.9594] | 4.5% |
| `hmm_drive_1` | 0.9814 | [0.9754, 0.9873] | 1.9% |
| `brmsmono` activation guard | 1.0027 | [0.9973, 1.0063] | parity |

The HMM run used 10 rounds x 0.5 seconds per measured gradient batch. The
secondary run used the same protocol. `brmsmono` used 20 rounds and 700,000
gradients per batch.

### Rebased PR smoke

The clean atomic implementation commit was rebuilt after rebasing and rerun
for 8 rounds x 0.25 seconds. The executable has the same SHA-256 as the
full-corpus executable, and all seven selected structurally active models
passed both order cohorts:

| Model | Candidate / baseline | 95% CI |
| --- | ---: | ---: |
| `iohmm_reg` | 0.7733 | [0.7679, 0.7744] |
| `accel_gp` | 0.8403 | [0.8379, 0.8433] |
| `accel_splines` | 0.8535 | [0.8472, 0.8560] |
| `hmm_drive_0` | 0.9381 | [0.9243, 0.9385] |
| `hmm_gaussian` | 0.9532 | [0.9477, 0.9614] |
| `soil_incubation` | 0.9504 | [0.9419, 0.9538] |
| `hmm_drive_1` | 0.9746 | [0.9640, 0.9809] |

The `brmsmono` structural guard was also rerun on the atomic commit: both arms
remain the same 43-op, 80-slot graph with no island.

The structural reductions explain the HMM results:

| Model | Forward instructions | Adjoint instructions | Registers |
| --- | ---: | ---: | ---: |
| `iohmm_reg`, opt-out | 31,968 | 30,472 | 43,488 |
| `iohmm_reg`, enabled | 22,484 | 20,988 | 32,008 |
| `hmm_gaussian`, opt-out | 26,956 | 25,460 | 25,479 |
| `hmm_gaussian`, enabled | 21,467 | 19,971 | 19,990 |
| `hmm_drive_0`, opt-out | 15,786 | 14,957 | 14,964 |
| `hmm_drive_0`, enabled | 13,711 | 12,882 | 12,889 |

Both accelerator models shrink an already-admitted island from 399 forward
and 399 adjoint instructions/registers to 266 of each. `soil_incubation` is a
directly lowered program rather than an island, showing that the pass is not
an HMM recognizer.

### Full corpus

The 8-round x 0.1-second screen produced:

- 120 models attempted;
- 119 models timed;
- zero performance failures;
- 114 immediate passes and five inconclusive/noisy cases;
- geometric mean 0.99227, 95% CI [0.99109, 0.99329];
- median 0.99986;
- p95 1.00503; and
- worst screen point estimate 1.01782, already below the 3% policy.

The five inconclusive models were rerun for 20 rounds x 0.5 seconds. All five
then passed both order cohorts:

| Model | Candidate / baseline | 95% CI |
| --- | ---: | ---: |
| `Rate_5_model` | 0.9991 | [0.9918, 1.0076] |
| `arma11` | 0.9958 | [0.9895, 1.0046] |
| `blr` | 0.9910 | [0.9719, 1.0030] |
| `garch11` | 0.9977 | [0.9946, 1.0005] |
| `logearn_logheight_male` | 1.0020 | [0.9948, 1.0097] |

Replacing the five screen point estimates with their decision-run estimates
gives a 0.99177 geometric mean, 0.99972 median, 1.0042 p95, and 1.00593 worst
point estimate. This replacement summary does not have a recomputed aggregate
confidence interval; the interval quoted above belongs to the original full
screen.

`sir` is the one untimed model: both arms reach its known invalid deterministic
benchmark point. It is not counted as a regression. The semantic corpus replay
includes it as the one identical both-fail case.

Tiny-model noise can look like a large win even when the artifacts are
identical (`dogs_hierarchical` is one example), so only the focused,
structurally active results above should be treated as attributable payoff.

## Preparation cost

Fresh-process preparation was measured separately for 20 paired ABBA/BAAB
rounds with `STANLI_PROFILE_PREP=1`. These are one-time file-to-bound-executor
costs, not per-gradient overhead:

| Model | Total preparation ratio | 95% CI |
| --- | ---: | ---: |
| `iohmm_reg` | 1.0062 | [1.0045, 1.0080] |
| `hmm_gaussian` | 1.0060 | [1.0048, 1.0072] |
| `hmm_drive_0` | 1.0155 | [1.0138, 1.0184] |
| `accel_gp` | 1.0002 | [0.9965, 1.0098] |
| `2pl_latent_reg_irt` | 1.0018 | [0.9978, 1.0042] |

The largest measured preparation cost is 1.55%, below the 3% per-model policy.
Using the median preparation delta and focused gradient saving, it amortizes in
roughly 1-121 gradients across the four measured winning models, well within a
normal warmup.

## Correctness and safety

- PosteriorDB semantic A/B: 119 runnable models, exact LP and all-gradient
  equality at the harness's deterministic point, zero flags; `sir` is the one
  identical known both-fail case.
- Unit tests compare graph/island values and generated adjoints on synthetic
  HMM, repeated-destination, and ranged-output fixtures.
- Release suite: 67/67 tests pass after rebasing and atomically squashing the
  candidate; the cross-path assertion is exactly the version on `main`.
- ASan+UBSan: seven targeted suites pass after the rebase
  (`test_pass_safety`, ODE program, MIR-program conformance, cross-path,
  island, adjoint, and `brmsmono`).
- Sanitized IOHMM feature-on/opt-out output is exactly identical.
- A high-effort Fable review independently checked the op table, read/write
  enumeration, generated-adjoint consume-and-zero behavior, activation path,
  ABI, raw measurements, and retained hashes. Its conditional-GO code and
  evidence conditions, including the atomic squash/rebase, are now satisfied
  locally; cross-platform CI remains a pre-merge condition.

Tests cover scalar and ranged forwarding, partial overlap, producer-input
aliasing, extra temporary reads, seeded/live-out temporaries, later output
overwrite, branch refusal, the profitable-island path, and the marginal
`brmsmono` activation guard. Fable also identified an inherited relaxation of
the cross-path island-switch floor; the original base assertion was restored
and passes in both Release and ASan+UBSan builds.

## Size and dependency constraints

Relative to current `origin/main`, the atomic implementation changes seven
files; the cross-path test is unchanged:

- production: +211 / -22 lines (189 net);
- tests: +298 / -5 lines; and
- total: +509 / -27 lines (482 net).

It adds no runtime dependency, no LLVM use, no executable-memory machinery,
and no architecture-specific code. It satisfies the project constraint that
LLVM may be used at build time but no outside library is required at runtime.

## Evidence and provenance

- Atomic implementation commit:
  `394e973a572e9821ac61f3b25e5851d760367384`
- Full-corpus measured production source commit:
  `46e6b158310afc043b939bcaf388ba7e239288a8`
- Release executable SHA-256:
  `3099a1ff369c6b008e6d02aa23dfd54004907e6713462d3ac3a552441fdc6084`
- Benchmark harness SHA-256:
  `ee4fc13f6c0a8e2af5489b9a4ec7f89f7469aa15d809f33de44be85f18d0be95`
- stanc executable SHA-256:
  `5c1e1c10bdd8eab806fce67376cd3caaeb55d6f71839151c0a3511a2f86b95d3`
- PosteriorDB commit:
  `28f8d3d6e975315f42aa274a8399f21e07a43b30`
- Targeted HMM results:
  `/private/tmp/destination-forward-targeted-gated-46e6b15.jsonl`
- Secondary focused results:
  `/private/tmp/destination-forward-secondary-gated-46e6b15.jsonl`
- Clean rebased-commit smoke results:
  `/private/tmp/destination-forward-pr-smoke-45acde5.jsonl`
- Full corpus and escalation results:
  `/private/tmp/destination-forward-corpus-gated-46e6b15.jsonl` and
  `/private/tmp/destination-forward-corpus-reruns-gated-46e6b15.jsonl`
- Paired preparation results:
  `/private/tmp/destination-forward-prep-gated-46e6b15.json`
- Activation audit and reusable payload:
  `/private/tmp/stanli-dest-activation-corpus.BUwU1Q/summary.json`
  (SHA-256
  `38dbaae0e0d89b4f547fa1b30025c4def2046d0bba60928284b61b9401acf1ce`)
- `brmsmono` decision run:
  `/private/tmp/brmsmono-destination-forward-gated-46e6b15.json`
- Full semantic replay:
  `/private/tmp/destination-forward-semantic-gated-46e6b15.txt`
  (SHA-256
  `36d556bd09af5f05eaaf190be5a7eb6b34d76fff8d492522fcfca4c23984ada8`)
- Frozen-candidate Release test log:
  `/private/tmp/destination-forward-release-tests-46e6b15.txt`
  (SHA-256
  `a02063f28f4d48f9ffb98347d8f6f7d34d9e01d562f4b8bf53a8fe80e419b602`)
- Frozen-candidate ASan+UBSan test log:
  `/private/tmp/destination-forward-sanitizer-tests-46e6b15.txt`
  (SHA-256
  `4f20e3dd819d16edf87415814163f2de20a902e9b50f749379272c2101656b5a`)
- Post-rebase 67-test Release log:
  `build-dest-forward/Testing/Temporary/LastTest.log`
  (SHA-256
  `be26c1b0697f6f474d8d52e343c7e24e3e942847939fc4a1a179019393c24ea6`)
- Post-rebase seven-test ASan+UBSan log:
  `build-dest-forward-san/Testing/Temporary/LastTest.log`
  (SHA-256
  `dbfa1de8bfccd36d8bb42ddca5a8cbe78d84c3c1e78c517631ab558895c65483`)
- Sanitized IOHMM parity manifest:
  `/private/tmp/destination-forward-iohmm-sanitized-parity-46e6b15.txt`
  (SHA-256
  `4845919e55987daa076e3fee0d6c1e06dd38248fa4fc3e8907a87cf99e07f7da`;
  its on/off outputs both hash to
  `6e9b8d79bd5796a302b7196d3ae214b68cf31c4e79ab9975055b4926cd8c985b`)
- Reproduced structural counts:
  `/private/tmp/destination-forward-structural-gated-46e6b15.txt`
  (SHA-256
  `54549904674ff4e325f1eb8e6c165e6200bf6d87fab4573cb1bdce978709f5e9`)
- High-effort Fable review:
  `/Users/xitrium/.claude/plans/act-as-a-skeptical-concurrent-squid.md`
  (SHA-256
  `a39d7c197987fac7610f7f07c8043689168485d0b2c8c96a42da2364090ff7d5`)

The activation audit self-records the exact `dump_islands` binary it used
(`825db7f8...`). That executable was subsequently rebuilt and now hashes to
`be405140...`; the audit remains tied to its recorded binary, and the focused
structural rows were reproduced after review with the retained inputs.

The full-corpus manifests record a clean candidate source worktree at
`46e6b15`; the rebased smoke manifest records a clean source worktree at
`45acde5`. The final atomic commit, `394e973`, differs from the smoke source
only by clang-format line wrapping, and all three produce the same
`bench_grad` hash. The harness came from a separate dirty source worktree and
the stanc source root was also dirty; their exact used files are content-hash
pinned above, so this provenance is content-based rather than a claim that
every source checkout was clean. The raw evidence paths are host-local
research artifacts and should be copied to durable CI or review storage before
the temporary workspace is removed. The atomic candidate is based directly on
current `origin/main` (`13f8c19`).

## PR and follow-on gates

1. Require normal cross-platform correctness CI before merge; local performance
   evidence is macOS arm64 only.
2. Copy the host-local benchmark and semantic artifacts to durable review or
   CI storage before removing the temporary workspace.
3. Keep the generic recurrence/helper experiment separate. Destination
   forwarding removes a large fraction of its motivating instruction traffic;
   any helper should now be judged only on its *marginal* gain over this pass,
   with the same 3% per-model corpus gate and a small implementation budget.
