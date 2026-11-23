# Issue #658: Pathfinder initialization for R WALNUTS

The direct user request is tracked in
[#658](https://github.com/seantalts/stanli/issues/658). R's
`sample_walnuts(pathfinder_init = list())` implementation and local validation
are included on this branch; merge and release are pending.

## Change and contract

The R interface reuses `.pathfinder_init_options()` and the existing
`stanli_r_pathfinder_inits` bridge, then passes the resulting unconstrained
starts through WALNUTS's existing explicit initialization support. No runtime
algorithm, compiler pipeline, C ABI or default sampler behavior changes.

The optional argument is appended after `refresh` to preserve positional
calls. It is mutually exclusive with `init`; an empty list uses the same
single-path Pathfinder defaults as `sample_model()`. The model is rebuilt
for random transformed data before initialization. One path keyed on the
first chain ID supplies all starts, so separate calls with different first
chain IDs need not reproduce the starts of a joint call. Without the new
argument, the existing chain/seed behavior remains unchanged.

Reference help and R README usage document these semantics. Production
changes are confined to `r/`.

## Validation

Baseline/runtime: `0eb880b9b9579cd28ca64c3afa4257cba9adca1e`, official
`abi6-0eb880b9b957-Darwin-arm64-threads` development library. The asset
SHA256 is recorded in the [collection](2026-11-23-social-feedback.md)'s
linked investigation records. Source R package 0.30.1 was installed privately
on R 4.6.1, macOS arm64.

The agent forced a bridge rebuild with `R CMD INSTALL --preclean
--install-tests` and inspected the complete compiler output: no compiler
diagnostics ([retained build log](data/issue658-2026-11-23/install.log)).
The WALNUTS, sampling and algorithms test files passed with
`STANLI_REQUIRE_RUNTIME=1`; every Pathfinder/WALNUTS runtime test ran.
The [focused-test log, losslessly compressed](data/issue658-2026-11-23/focused-tests.log.gz) retains
the results, skip reason and sampler messages.
WALNUTS has 116 passing expectations, including default/custom options,
multiple chains/nondefault chain IDs, exact draw/sampler equality against
explicit Pathfinder-generated starts, reproducibility, transformed-data
seeds, caller R RNG preservation, error propagation and `init` exclusion.

One pre-existing sampling test skipped because
`STANLI_TEST_PORTABLE_COMPILER` did not name a standalone `stanli-compile`.
Short-run sampler adaptation warnings were observed and are separate from
compiler diagnostics. Roxygen2 8.1.0 verified the reference help matches its
source. The lead's clang-format 22.1.8 check and whitespace checks passed.

Reproduce the focused tests after installing current `r/` into a private
library:

```sh
R_LIBS_USER=/path/to/private-library STANLI_RUNTIME=/path/to/libstanli.dylib \
  STANLI_REQUIRE_RUNTIME=1 Rscript -e \
  'testthat::test_dir("r/tests/testthat", filter="^(walnuts|sampling|algorithms)$", package="stanli", load_package="installed")'
```

## Integrated full-suite validation

At combined-branch commit `f047d5d56c7cf5771afe4bc41f135332bc2b492d`,
the R bridge was installed privately again with `--preclean --install-tests`.
Its complete [build log](data/issue658-2026-11-23/integration-install.log)
contained no compiler diagnostics. Native production source matches the
validated candidate runtime and fresh CLI; their hashes, exact commands and
private library path are in [integration provenance](data/issue658-2026-11-23/integration-provenance.json).

With `STANLI_REQUIRE_RUNTIME=1`, the full 36-file suite passed **2,210
expectations across 293 test blocks, with zero failures, errors or test
warnings**. All 116 WALNUTS expectations and the fresh `STANLI_RUN` CSV
oracle's 140 expectations ran. See the [full log](data/issue658-2026-11-23/integration-full-suite.log.gz)
and [per-test results](data/issue658-2026-11-23/integration-full-suite-results.json.gz).

Three optional tests skipped: installed brms lacks the stanli backend, and
no standalone `stanli-compile` was available for the native include-path and
O1-warning tests. Bundled JavaScript/V8 tests ran; CI must cover the two
standalone cases. This record does not claim PR CI passed.

The first run passed the same tests but its post-test exporter failed on a
list column ([original log](data/issue658-2026-11-23/integration-first-run.log.gz)).
Only the cache-local exporter was corrected; the identical full suite reran
and exited zero. Sampler messages are retained separately from compiler/test
diagnostics. The R-only feature itself requires no compiler-pipeline benchmark.

After the native replay-policy repair for PR #659, the same full suite ran
again against the final runtime and CLI: 2,210 passes, zero failures/errors/
test warnings and the same three optional skips. The CSV oracle ran. See the
[final full log](data/issue658-2026-11-23/integration-v4-full-suite.log.gz)
and [final results](data/issue658-2026-11-23/integration-v4-full-suite-results.json.gz).
R source was unchanged by this repair.
