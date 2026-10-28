# What a Stan program prints, and what the sampler says, reaches R's own
# output stream, so capture.output(), sink() and knitr chunks see it.

printing_code <- "
  transformed data { print(\"td says hello\"); }
  parameters { real mu; }
  model { print(\"model mu=\", mu); mu ~ normal(0, 1); }
  generated quantities { print(\"gq mu=\", mu); }"

test_that("print() from the model is captured at every entry point", {
  skip_without_runtime()
  text <- capture.output(model <- stanli_model(code = printing_code))
  expect_identical(text, "td says hello")

  text <- capture.output(invisible(log_prob_grad(model, 0.5)))
  expect_identical(text, "model mu=0.5")

  text <- capture.output(fit <- sample_model(model, chains = 1, warmup = 3,
                                             samples = 2, refresh = 0,
                                             seed = 1))
  expect_true(any(startsWith(text, "model mu=")))
  expect_identical(sum(startsWith(text, "gq mu=")), 2L)
  expect_true(all(grepl("^(model|gq) mu=\\S+$", text)))

  text <- capture.output(invisible(optimize_model(model, seed = 1)))
  expect_true(any(startsWith(text, "model mu=")))

  text <- capture.output(invisible(suppressWarnings(
    variational_model(model, seed = 1, iter = 200, draws = 5, refresh = 0))))
  expect_true(any(startsWith(text, "model mu=")))

  text <- capture.output(invisible(pathfinder_model(model, seed = 1, draws = 5,
                                          refresh = 0)))
  expect_true(any(startsWith(text, "model mu=")))

  text <- capture.output(invisible(laplace_model(model, seed = 1, draws = 3,
                                       refresh = 0)))
  expect_true(any(startsWith(text, "gq mu=")))
})

test_that("print() from parallel chains arrives as whole lines", {
  skip_without_runtime()
  capture.output(model <- stanli_model(code = printing_code))
  text <- capture.output(invisible(sample_model(model, chains = 4, warmup = 50,
                                      samples = 50, refresh = 0, seed = 3)))
  text <- text[nzchar(text)]
  expect_gt(length(text), 400L)
  expect_true(all(grepl("^(model|gq) mu=\\S+$", text)))
  expect_identical(sum(startsWith(text, "gq mu=")), 200L)
})

test_that("print() and progress share one ordered stream", {
  skip_without_runtime()
  capture.output(model <- stanli_model(code = printing_code))
  text <- capture.output(invisible(sample_model(model, chains = 1, warmup = 4,
                                      samples = 3, refresh = 1, seed = 2)))
  progress <- grep("Iteration:", text, fixed = TRUE)
  expect_gt(length(progress), 0L)
  expect_true(any(startsWith(text[seq_len(progress[1])], "model mu=")))
  expect_true(any(startsWith(text[progress[1]:max(progress)], "gq mu=")))
  expect_gt(grep("Elapsed Time", text, fixed = TRUE)[1], max(progress))
})

test_that("sink() sees the same text as capture.output()", {
  skip_without_runtime()
  path <- tempfile()
  on.exit(unlink(path))
  sink(path)
  tryCatch(model <- stanli_model(code = printing_code), finally = sink())
  expect_identical(readLines(path), "td says hello")
})
