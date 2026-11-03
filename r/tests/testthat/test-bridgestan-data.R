bridgestan_code <- "
  data { int<lower=0> N; vector[N] y; }
  parameters { real mu; }
  model { mu ~ normal(0, 5); y ~ normal(mu, 1); }"

parse_bridgestan_data <- function(text) {
  skip_if_not_installed("jsonlite")
  jsonlite::fromJSON(text, simplifyVector = FALSE)
}

test_that("stanli_build_id names the loaded runtime", {
  skip_without_runtime()
  id <- stanli_build_id()
  expect_type(id, "character")
  expect_length(id, 1L)
  expect_true(nzchar(id))
})

test_that("bridgestan_data adds the model to the data object", {
  skip_without_runtime()
  model <- stanli_model(code = bridgestan_code,
                        data = list(N = 3L, y = c(0.5, -1, 2)))
  parsed <- parse_bridgestan_data(bridgestan_data(model))
  expect_identical(names(parsed)[1L], "__stanli")
  expect_identical(parsed$`__stanli`$build_id, stanli_build_id())
  expect_identical(parsed$`__stanli`$name, "stanli_model")
  expect_identical(parsed$N, 3L)
  expect_equal(unlist(parsed$y), c(0.5, -1, 2))
  mir <- parsed$`__stanli`$mir
  expect_type(mir, "character")
  expect_gt(nchar(mir), 100L)
  rebuilt <- stanli_model(mir = mir, data = list(N = 3L, y = c(0.5, -1, 2)))
  expect_identical(log_prob_grad(rebuilt, 0.3), log_prob_grad(model, 0.3))
})

test_that("bridgestan_data handles a model with no data", {
  skip_without_runtime()
  model <- stanli_model(code = "parameters { real x; } model { x ~ normal(0, 1); }")
  parsed <- parse_bridgestan_data(bridgestan_data(model))
  expect_named(parsed, "__stanli")
  expect_identical(parsed$`__stanli`$build_id, stanli_build_id())
})

test_that("bridgestan_data passes a MIR model through unchanged", {
  skip_without_runtime()
  fast <- stanli_model(code = bridgestan_code, fast_math = TRUE,
                       data = list(N = 2L, y = c(1, 2)))
  expect_true(fast$source$is_mir)
  parsed <- parse_bridgestan_data(bridgestan_data(fast))
  expect_identical(parsed$`__stanli`$mir, fast$source$code)
})

test_that("json_string escapes quotes, backslashes and control characters", {
  skip_if_not_installed("jsonlite")
  text <- "a \"q\" \\ b\n\tc\r\u0001 \u00e9"
  encoded <- stanli:::json_string(text)
  expect_identical(jsonlite::fromJSON(encoded), text)
})

test_that("bridgestan_data keeps the content of a data file", {
  skip_without_runtime()
  path <- tempfile(fileext = ".json")
  writeLines(c("{", "  \"N\": 1,", "  \"y\": [0.25]", "}"), path)
  on.exit(unlink(path), add = TRUE)
  model <- stanli_model(code = bridgestan_code, data = path)
  parsed <- parse_bridgestan_data(bridgestan_data(model))
  expect_identical(parsed$N, 1L)
  expect_equal(unlist(parsed$y), 0.25)
  expect_error(bridgestan_data(list()), "stanli_model")
})
