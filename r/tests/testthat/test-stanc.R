# The one path that works without a runtime, and so the only thing CRAN's
# check farm will actually execute: stanc3 compiled to JavaScript, run
# through V8. It is what makes the package shippable there at all, so it
# is worth a test that does not skip on the machines that matter.

test_that("the bundled JavaScript compiler produces MIR", {
  skip_if_not_installed("V8")
  skip_if_not_installed("jsonlite")
  skip_if(!nzchar(stanli:::stanc_js_path()) ||
            !file.exists(stanli:::stanc_js_path()),
          "stanc.js is not in this installation")

  mir <- stanli:::mir_from_js("
    data { int<lower=0> N; vector[N] y; }
    parameters { real mu; real<lower=0> sigma; }
    model { y ~ normal(mu, sigma); }")

  expect_type(mir, "character")
  # An s-expression naming the model's own symbols, not just any output:
  # a compiler that silently emitted an empty program would still be a
  # non-empty string.
  expect_match(mir, "\\bmu\\b")
  expect_match(mir, "\\bsigma\\b")
  expect_match(mir, "normal")
})

test_that("a model that does not typecheck is an error, not empty MIR", {
  skip_if_not_installed("V8")
  skip_if_not_installed("jsonlite")
  skip_if(!nzchar(stanli:::stanc_js_path()) ||
            !file.exists(stanli:::stanc_js_path()),
          "stanc.js is not in this installation")

  # `y` is undeclared. stanc reports this through `errors` rather than a
  # thrown exception, so the wrapper has to look for it: without that
  # check a broken model reaches the lowering pass as NULL.
  expect_error(
    stanli:::mir_from_js("parameters { real mu; } model { mu ~ normal(y, 1); }"),
    "stanc")
})

test_that("the runtime asset name is one of the five that get published", {
  # r/R/install.R builds this from R's own spelling of the platform, and
  # the release workflow builds the tarball names from a matrix. They are
  # two lists that have to stay equal, and only one of them is checked by
  # a compiler.
  published <- c("stanli-runtime-darwin-arm64.tar.gz",
                 "stanli-runtime-darwin-x86_64.tar.gz",
                 "stanli-runtime-linux-x86_64.tar.gz",
                 "stanli-runtime-linux-arm64.tar.gz",
                 "stanli-runtime-windows-x86_64.tar.gz")
  expect_true(stanli:::runtime_asset() %in% published)
})
