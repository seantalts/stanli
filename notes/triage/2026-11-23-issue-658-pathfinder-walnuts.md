# Issue #658: Pathfinder initialization for R WALNUTS

The direct user request is tracked in
[#658](https://github.com/seantalts/stanli/issues/658). R's
`sample_walnuts(pathfinder_init = list())` is implemented locally. The issue
remains open; this change has not been pushed, merged or released.

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

The full R suite's CSV-oracle checks with a fresh `STANLI_RUN` and the
standalone portable-compiler case remain integration validation. This record
does not claim those checks or PR CI ran. The R-only change does not require
a compiler-pipeline performance benchmark.
