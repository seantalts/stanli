# General runtime fixes extracted from the ISA investigation

The final comparison uses upstream `14e14253a3a4d8908deaf222c32da59d2ec40b51`
and candidate `7c6d70ecea908f672cd422591b2edb628f73a91b`. The candidate
retains the existing minimum ISA and floating-point flags. Two changes reuse
existing runtime infrastructure:

- The private function registry uses the Boost flat map already used in
  lowering, with one inline overload pointer per name. Canonical specification
  storage, overload insertion order, family views, and the public API remain
  unchanged. Reserving 512 specification entries avoids the current growth
  from 384 to 768. Earlier allocation attribution measured 52 index allocations
  versus 864 and approximately 4.8 KiB more retained requested storage overall;
  those are allocator requests, not RSS measurements.
- The existing late forward-callback resolver selects an arity-specialized
  version of the original `ADD_N` loop. Every version accumulates from zero
  in input order and writes only after reading all inputs. Dynamic lengths,
  unrecognized variants, and custom kernel registrations keep their original
  callback. This extraction handles upstream's scalar inputs only; the
  research branch's separate indexed-input extension is excluded.

The ADD_N regression test covers all supported arities, eight buffer
alignments, output/input aliasing, and fallback selection. The existing
registry test checks every overload and canonical pointer identity.

## Rejected inclusion: constant sharing

Against the earlier `cc99a937` baseline, the initial three-fix candidate
passed all 406 CTest cases and a strict
353-model, three-point comparison with unchanged upstream, with zero output
changes. Recorded CmdStan replay passed 350/353; three GP failures reproduced
on unchanged upstream with identical errors on this host.

The full performance run was stopped with its partial evidence retained after
five warm-gradient crossings. A six-round independent-process comparison of
baseline, each individual fix, the combination, and an identical-binary
control confirmed slowdowns from constant sharing: Rate_2_model +4.994%,
aalto_grp_aov +2.574%, aalto_grp_prior_mean +4.501%, and arK +6.443%. The
combination also slowed GLM_Poisson_model +6.396%. These models contain no
register islands in their printed graphs; the constant-sharing transform does
not execute in their warm paths. The cause remains unresolved; placement/layout effects are a hypothesis. Mb_model improved 8.91% with constant sharing alone, but that gain
does not excuse the other regressions. Constant sharing is therefore excluded
from this PR and retained in the ISA research artifacts for investigation.

The registry-plus-ADD_N combination passed the same six-case confirmation:
GLM_Poisson_model -0.095%, Rate_2_model +0.187%, aalto_grp_aov -1.031%,
aalto_grp_prior_mean -2.043%, arK +0.048%, and Mb_model +0.313%. Identical-binary
controls ranged from -0.298% to +1.229%; this is focused evidence, not a
full-corpus pass. The complete fresh comparison and its limitations follow below.


## Fresh validation and full performance comparison

Both revisions were clean-built on the same Ubuntu 24.04 i9-13900K host with
Clang 18.1.3, Release `-O3 -DNDEBUG`, `-ffp-contract=off`, density oracle OFF,
and shared test runtime ON. Neither build enables AVX2 or x86-64-v3. Final
build output contained no compiler warnings or errors. All 406 CTest cases
passed. Strict differential replay covered 353 models at three points
(1,059,605 non-NaN values, 531 NaNs, six rejected points) with zero changes,
including signed zero and non-finite classifications. Recorded CmdStan replay
passed 350/353. The existing failures in i320_gp_expquad, i320_gp_matern32,
and sw_gp reproduced with identical errors on fresh baseline; tolerances were
not changed. Required PR CI passed, including Linux/Windows and interfaces.

The library grew from 49,965,032 to 49,975,816 bytes: +10,784 bytes (0.022%).
No fresh peak-RSS or complete-inference claim is made.

The full application corpus ran six alternating baseline/candidate process
pairs, pinned to P-core 8 with no concurrent builds. Warm gradients used the
shared C-ABI driver/timer, 200 ms warmup and 250 ms measurement. Source and
preparation were measured separately in fresh processes. The source producer
was identical for both labels. The fixed-work estimate is source + preparation
+ 20,000 gradients. All 343 models were attempted; 340 have complete phase
results. dogs_log, s2_invgaussian, and sir failed on both revisions and remain
unresolved, not passing observations.

Across complete rows, median changes were source +0.122%, preparation -1.633%,
warm gradients -0.060%, and fixed work -0.060%. These aggregate numbers do not
clear individual crossings. The original run had four source, two preparation,
seven warm-gradient, and six fixed-work crossings at or above 2%.

One predeclared twelve-round confirmation covered every crossing with a third,
identical-baseline-binary control. No candidate phase slowdown remained at 2%:

| Warm-gradient case | Full run | Confirmation | A/A control |
| --- | ---: | ---: | ---: |
| arma11 | +3.073% | -0.705% | +0.079% |
| ch09_m9_4 | +2.688% | +1.471% | +2.708% |
| ch14_m14_1 | +2.405% | +1.343% | +0.652% |
| covid19imperial_v2 | +4.139% | -12.043% | -4.124% |
| ctsem_ctsm_400 | +2.907% | -4.975% | -3.030% |
| ldaK5 | +2.149% | -2.199% | -0.031% |
| logmesquite_logvolume | +4.750% | -0.650% | +3.442% |

The original source crossings confirmed at -0.381%, -0.234%, +0.375%, and
+0.674% respectively (ch09_m9_2, ch13_m13_4b, diamonds, normal_mixture).
Preparation confirmed at -0.019% for covid19imperial_v2 and +1.261% for
nn_rbm1bJ100. Recalculated fixed-work estimates for the six original crossings
also stayed below 2% using their confirmation phase measurements.

This supports landing the two changes under the user's explicit allowance
for reasonable confidence. It is **not** a complete no-regression proof:
four warm A/A controls exceed 2% in magnitude, those signals remain
inconclusive, and three shared execution failures remain unresolved. Neither
the original crossings nor the noisy controls are discarded. Constant sharing
is excluded because its separate regressions were confirmed.

## Retained evidence and reproduction

[Evidence directory](data/2026-10-15-general-runtime-fixes/) contains gzip
copies of unmodified raw sample/result JSON, full per-model comparisons,
input/binary manifests, exact executed commands, the confirmation plan,
strict numerical results, and initial ablations. `SHA256.json` records file
hashes. `full-commands.json.gz` and `confirm-commands.json.gz` identify the
commands; manifests record input hashes and binary identities. The original
run directories remain under
`/home/xitrium/claude/stanli-isa-20261013/output/isa-continuation/` as
`general-pr-14e-performance`, `general-pr-14e-confirm`, and
`general-pr-14e-validation`, including process output and failure logs.

Dependency pins were shared: Math `5252d51d47c1d5e78005fc043ad996fad6dd8da8`,
CmdStan `d3d5df6a22565edbe13edbd4eb40762cc8c5a4d6`, Stan
`a6806ef8477a7b5f65b27449ec33528c162bc024`, stanc
`d58446e631b02cacc5355e373defc6092a684554`, and posteriordb
`28f8d3d6e975315f42aa274a8399f21e07a43b30`. Both builds used the same existing
Math adjoint-CVODES patch that zeroes `qBdot` before accumulation; this patch
was neither introduced nor changed by the PR. The producer SHA-256 was
`434a2e4c6fe9e8f2eb7b85f6f84bb262f0586abef51467396f346a09b04fdbc1`.
