test_that("STANLI_REQUIRE_RUNTIME=1 turns a missing runtime into a failure", {
  old <- Sys.getenv("STANLI_REQUIRE_RUNTIME", unset = NA)
  on.exit(if (is.na(old)) Sys.unsetenv("STANLI_REQUIRE_RUNTIME")
          else Sys.setenv(STANLI_REQUIRE_RUNTIME = old))
  Sys.unsetenv("STANLI_REQUIRE_RUNTIME")
  expect_condition(skip_without_runtime(available = FALSE), class = "skip")
  Sys.setenv(STANLI_REQUIRE_RUNTIME = "1")
  expect_error(skip_without_runtime(available = FALSE),
               "STANLI_REQUIRE_RUNTIME")
  expect_no_condition(skip_without_runtime(available = TRUE))
})

test_that("STANLI_REQUIRE_NUTPIER=1 turns a missing nutpieR into a failure", {
  old <- Sys.getenv("STANLI_REQUIRE_NUTPIER", unset = NA)
  on.exit(if (is.na(old)) Sys.unsetenv("STANLI_REQUIRE_NUTPIER")
          else Sys.setenv(STANLI_REQUIRE_NUTPIER = old))
  Sys.unsetenv("STANLI_REQUIRE_NUTPIER")
  expect_condition(skip_without_nutpie(installed = FALSE), class = "skip")
  Sys.setenv(STANLI_REQUIRE_NUTPIER = "1")
  expect_error(skip_without_nutpie(installed = FALSE),
               "STANLI_REQUIRE_NUTPIER")
})
