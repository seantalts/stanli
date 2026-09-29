# Historical Aalto fixture experiment — 2026-09-14

This page records the experiment at revision `e46e360f` and its preceding
comparisons. The measured values, protocol and validation counts below belong
to those revisions. New numerical replay, sampling smoke tests and benchmarks
use the [common corpus paths](README.md).

**Pareto reaches 1.072x vectorized CmdStan end to end. All 13 models pass.**
The experiment required Pareto >= 1.0x and every other model >= 0.5x.
The three-point CmdStan oracle checks 6,609 scalar values, with worst scaled
error 8.30e-15; all generated-output, sampling CSV and posterior mean checks pass.

Apple M3 Ultra, macOS arm64, Apple Clang 21, Release. CmdStan uses `--O1`,
`-O3`, and `-ffp-contract=off`. Each current median contains five alternating
pairs following one warmup pair: 1000 warmup + 1000 saved draws and complete
17-digit CSV files. Stanli includes source compilation, JSON preparation,
NUTS and generated quantities. CmdStan's executable is already compiled;
its fresh build time is recorded separately. No models or output phases are
excluded. These are the supplied teaching fixtures, seven with synthetic data.

[Final observations](pareto-benchmark-results.json) include hashes, raw
samples, dispersion, phase measurements and toolchain identities. The previous
PR results are retained in [optimized observations](optimized-benchmark-results.json),
and the original pre-fix baseline in [baseline observations](benchmark-results.json).
The previous column below is historical; it is not a paired revision comparison.

| Model | Previous PR Stanli (ms) | Current Stanli (ms) | CmdStan (ms) | Speed vs CmdStan | Required floor |
|---|---:|---:|---:|---:|---:|
| aalto_bern | 17.63 | 12.08 | 16.25 | 1.345x | 0.5x |
| aalto_binom | 15.86 | 11.24 | 12.94 | 1.151x | 0.5x |
| aalto_binom2 | 18.39 | 12.94 | 16.33 | 1.262x | 0.5x |
| aalto_binomb | 16.37 | 10.94 | 12.92 | 1.181x | 0.5x |
| aalto_gpareto | 48.68 | 30.75 | 32.97 | 1.072x | 1.0x |
| aalto_grp_aov | 23.03 | 16.54 | 21.74 | 1.315x | 0.5x |
| aalto_grp_prior_mean | 28.82 | 24.79 | 33.49 | 1.351x | 0.5x |
| aalto_grp_prior_mean_var | 62.42 | 64.09 | 91.37 | 1.425x | 0.5x |
| aalto_lin | 27.17 | 22.88 | 33.56 | 1.467x | 0.5x |
| aalto_lin_std | 23.44 | 18.23 | 30.41 | 1.668x | 0.5x |
| aalto_lin_std_t | 29.52 | 23.13 | 34.42 | 1.488x | 0.5x |
| aalto_poisson_hurdle | 665.37 | 653.72 | 871.43 | 1.333x | 0.5x |
| aalto_poisson_simple | 97.69 | 94.72 | 245.24 | 2.589x | 0.5x |

Pareto's current medians are **30.746 ms Stanli / 32.974 ms CmdStan**.
Stanli ranges from 30.084–31.774 ms (MAD 0.662 ms); CmdStan from
31.223–33.419 ms (MAD 0.445 ms). An earlier eleven-pair diagnostic measured
1.110x. The final five-pair gate is authoritative; the margin over parity is
modest and should remain guarded.

## General fixes

- Load built-in signature sets only when used; memoize immutable overload
  resolution within each compilation with a bounded, exception-safe cache.
- Return an ordinary unknown result from speculative constant probes instead
  of repeatedly throwing and catching compiler exceptions. Preserve lazy
  expression evaluation and parameter-independent shape queries.
- Generate reverse code for forward-only branches by recording executed
  blocks. Retain replay for loops, unsupported active derivatives and
  unproven initialization/aliasing. Generated quantities need no reverse code.

A [matched five-pair revision comparison](pareto-matched-revision-results.json)
agrees with the explanation: Pareto improves **1.790x** over PR commit
`4c25331f`, from 49.456 to 27.632 ms. Preparation falls 25.664 → 8.450 ms
and sampling 14.898 → 8.403 ms. GQ/CSV output measures 4.409 → 4.961 ms;
the overall gain comes from preparation and reverse evaluation. Differences
between these absolute times and the CmdStan run are why each comparison
uses its own paired measurements. Every educational model's complete elapsed
median improves in that revision comparison, but small differences such as
hurdle-Poisson's 1.018x should not be interpreted as statistically established.

One separate `/usr/bin/time -l` probe measured maximum resident memory of
27,148,288 → 17,416,192 bytes for Pareto with the same sampling settings.
The generated island uses ten path flags and 109 adjoint instructions, retaining
its three kernel calls. No compiler/runtime optimization recognizes a model,
source name or variable spelling.

The earlier PR fixes remain: compiled scalar integer RNG control, shared
Poisson/Student-t/Bernoulli-logit RNG kernels, unused-procedure pruning after
inlining, and buffered exact 17-digit CSV formatting. Their original evidence
is preserved in [the first investigation](../../docs/superpowers/plans/2026-09-14-educational-performance.md).

## Recorded validation and historical reproduction

- 247/247 Release CTests, plus the updated performance-floor unit test.
- 254/254 corpus models, 976,394 values under the existing numerical policy.
- Native/JavaScript producer bytes agree in general and model-only modes.
- ASan+UBSan adjoint, program-conformance and write-array tests pass.
- [52 complete CSV files](pareto-revision-parity.json) match the prior PR
  bitwise across 13 models and four seeds. The additional matched benchmark
  confirms byte equality for all six of its seeds as well.
- Adjoint tests cover reused branch flags, nested paths, conditional copies,
  overwritten conditions/values, kernel calls, four-argument densities, dead
  invalid expressions, rejects, active-extrema refusal and 256 dyadic path cases.

The commands below require the recorded revision `e46e360f`; the specialized
runner and performance target have since been retired. They are retained to
identify how this experiment was produced, not as current instructions.

```sh
python3 tools/check_educational.py --benchmark --repetitions 5 \
  --output build-rel/educational-pareto-final/results.json
ctest --test-dir build-rel -j8 --output-on-failure
python3 tools/verify_refs.py deps/posteriordb \
  --check build-rel/stanli_check --jobs 4
```

At that revision, CTest used recorded references and the separate
`check_educational_performance` target required CmdStan and a quiet machine. See the [Pareto research ledger](../../docs/superpowers/plans/2026-09-14-pareto-parity.md)
for proof obligations, ablations and conservative fallbacks. These fixtures
and smoke tests do not establish full-size course-data performance or convergence.
