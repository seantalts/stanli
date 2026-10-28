# What a user reads when a model fails to compile or data is missing.

test_that("a data path that does not exist is reported as a missing file", {
  skip_without_runtime()
  code <- "data { real y; } parameters { real mu; } model { y ~ normal(mu, 1); }"
  missing <- file.path(tempdir(), "no-such-dir", "data.json")
  expect_error(stanli_model(code = code, data = missing),
               paste0("data file not found: ", missing), fixed = TRUE)
  expect_error(stanli_model(code = code, data = "data.json"),
               "data file not found: data.json", fixed = TRUE)

  inline <- stanli_model(code = code, data = "{\"y\": 1.5}")
  expect_s3_class(inline, "stanli_model")
  path <- tempfile(fileext = ".json")
  on.exit(unlink(path), add = TRUE)
  writeLines("{\"y\": 1.5}", path)
  expect_s3_class(stanli_model(code = code, data = path), "stanli_model")
})

test_that("a syntax error names the model file the user gave", {
  skip_without_runtime()
  dir <- tempfile("stanli-msg-")
  dir.create(dir)
  on.exit(unlink(dir, recursive = TRUE), add = TRUE)
  path <- file.path(dir, "broken.stan")
  writeLines("parameters { real mu } model { }", path)
  err <- tryCatch(stanli_model(file = path), error = conditionMessage)
  expect_true(is.character(err))
  expect_match(err, "broken.stan", fixed = TRUE)
  expect_no_match(err, "'string'", fixed = TRUE)
})

test_that("a compiler warning reaches the user once", {
  skip_without_runtime()
  seen <- character()
  withCallingHandlers(
    stanli_model(code = ""),
    warning = function(w) {
      seen <<- c(seen, conditionMessage(w))
      invokeRestart("muffleWarning")
    })
  expect_length(grep("Empty model", seen, fixed = TRUE), 1L)
})
