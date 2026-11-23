# Issue #656: first covariance/Cholesky investigation

For the subsequent synthetic structural search and standalone resource-cost
reproducer, see [round 2](2026-11-23-issue-656-round2.md). The evidence below
records the narrower first round.

## Current decision

The reported low-level bit-length error is **not reproduced**. Custom
covariance assembly and Cholesky decomposition work in the bounded probes
below on current main, including through R. No production fix is justified
by this evidence. Keep [#656](https://github.com/seantalts/stanli/issues/656)
open for the original source/data, exact exception and version information.
A successful diagnostic substitute is not evidence that the original
reported model is fixed.

## Provenance and scope

- Fetched remote-default baseline:
  `0eb880b9b9579cd28ca64c3afa4257cba9adca1e`. The lead created this clean
  worktree explicitly from that ref; ancestry was verified locally.
- Official dev runtime: `abi6-0eb880b9b957-Darwin-arm64-threads`, downloaded
  by the lead from `dev-0eb880b9b957`. The lead verified archive SHA256
  `8d955fe96c6e7b2df499ada2f81a0f49c845c4b560042ce2068adba2a7c927c4`.
  The [raw result](data/2026-11-23-issue-656.json) records the library hash,
  model hash, toolchain revisions and exact reference build commands.
- Independent oracle: pinned CmdStan 2.40.0 at
  `d3d5df6a22565edbe13edbd4eb40762cc8c5a4d6`, Stan Math at
  `5252d51d47c1d5e78005fc043ad996fad6dd8da8`, upstream stanc3 v2.40.0.
  The reference driver uses `-O1 -ffp-contract=off`, with no custom compiler
  or dependency patches. This is a correctness diagnostic, not a speed
  comparison. Both compilation logs were empty: no compiler diagnostics.
- R 4.6.1, fresh source stanli R package 0.30.1 from the same baseline,
  installed in the sibling issue-657 investigation's temporary R library.
- An existing shared `build-rel` library was inspected but rejected for
  current-main validation: it reports
  `abi1-1d9eeef43bdb-Darwin-arm64-threads`.

## Public-source investigation

The [reported failure](https://bsky.app/profile/rmcelreath.bsky.social/post/3n2vgbotnl22z)
does not include a program or exact diagnostic. The public rethinking
Experimental branch was inspected at
[`08db6d4c08993f2570121c8584e0fe8f344ed1e3`](https://github.com/rmcelreath/rethinking/tree/08db6d4c08993f2570121c8584e0fe8f344ed1e3).
Its [`R/ulam_stanli_tests.R`](https://github.com/rmcelreath/rethinking/blob/08db6d4c08993f2570121c8584e0fe8f344ed1e3/R/ulam_stanli_tests.R)
contains ordinary varying-effect tests, not the unshared custom covariance
failure. Its `ulam` integration calls `stanli_model` and `sample_model`.

Searches for a literal bit-length diagnostic in project compiler/runtime,
R/Python interface code, Stan Math source, and strings in the current runtime
found no match. The existing stanc3 dependency directory contains compiler
artifacts rather than a source checkout, so a requested source search there
could not run. Without the exact exception these searches do not identify
a failing component.

The existing book fixture `tests/rethinking/ch14_m14_8nc.stan` already
assembles `SIGMA` in a user function using indexed symmetric writes, takes
`cholesky_decompose(SIGMA)`, and multiplies the factor by a latent vector.
It prepared successfully and produced finite densities/gradients at all
three standard fixed parameter points through the current C ABI. The R
check at the zero point also succeeds. This is an existing public example,
not a recovered reproduction of the social report.

## Evaluator and observations

The [diagnostic Stan program](../../harnesses/diagnostics/custom_covariance.stan)
builds a symmetric squared-exponential covariance matrix in a user-defined
function, takes its Cholesky factor in transformed parameters, and uses
`L * z` in a normal likelihood. These structural choices exercise a
plausible path from the report without identifying any particular model.

The [Python C-ABI harness](../../harnesses/diagnostics/custom_covariance.py)
separates source compilation from preparation and compares the log density
and every gradient component with an independent CmdStan reference at the
corpus's three standard deterministic points. Its invalid variants check
observable rejection behavior. Timings in the raw JSON are single-run
diagnostics only.

| Case | Points | Observation |
| --- | ---: | --- |
| Valid covariance, N=1 | 3 | Density and all gradients bitwise equal to CmdStan. |
| Valid covariance, N=3 | 3 | Density and all gradients bitwise equal to CmdStan. |
| Valid covariance, N=40 | 3 | Two points bitwise equal; one differs by at most 1 ULP (maximum scaled error 1.37e-16). Exercises the Cholesky reverse branch above the N=35 threshold. |
| N=3, negative diagonal | 3 | Both reject; Stanli reports `cholesky_decompose: Matrix m is not positive definite`. |
| N=3, asymmetric matrix | 3 | Both reject; Stanli reports non-symmetry and the differing entries. |

The [R smoke harness](../../harnesses/diagnostics/custom_covariance.R)
checks finite densities/gradients at N=1, 3 and 40, the two invalid cases,
the existing rethinking fixture, and 25 warmup plus 25 retained draws at
N=3. All completed; [retained output](data/2026-11-23-issue-656-r.log).
The deliberately short run emits Stan's adaptation-window warning; it is
a sampling/interface smoke check, not posterior validation.

An initial R probe used unshaped length-one numeric vectors at N=1 and
failed data initialization because they serialize as scalars. Preserving
the vector dimension with `array(x, n)` makes the probe match its declared
Stan data shape. The [initial output](data/2026-11-23-issue-656-r-scalar-input.log)
is retained. This was a probe-input mismatch, not a reproduction of a
Cholesky or bit-length failure, and no implementation change was made for
it.

## Hypotheses and next decision

1. **General covariance/Cholesky support is missing:** contradicted by the
   existing book fixture and these small/large, valid/invalid probes.
2. **A model-specific shape/construction interaction or interface/version
   issue triggers the error:** remains untested. The exact generated model
   and error are needed to distinguish compilation, preparation, data
   conversion, evaluation or sampling/output phases.

Next discriminating input: original generated Stan source, minimal data,
full exception/traceback, runtime build ID, package version and failing
platform. Then reduce the actual failing phase before proposing a generic
fix. No bit-length root cause, sampler parity, per-draw-output parity or
universal numerical bound is established by this first round.

## Reproduction commands

From the repository root, with paths adjusted to the matching runtime and
pinned dependency checkouts:

```sh
python3 harnesses/diagnostics/custom_covariance.py \
  --runtime /path/to/libstanli.dylib \
  --cmdstan /path/to/cmdstan \
  --stanc /path/to/stanc-pinned \
  --work .cache/issue656/ref \
  --output .cache/issue656/results.json
R_LIBS=/path/to/fresh-r-library STANLI_RUNTIME=/path/to/libstanli.dylib \
  Rscript harnesses/diagnostics/custom_covariance.R
```

No runtime/compiler code was changed. No PR or issue closure is warranted
from this diagnostic evidence. Harness syntax and `git diff --check` pass.
