url <- "https://github.com/seantalts/stanli/releases/download/v9.9.9/stanli-runtime-darwin-arm64.tar.gz"

test_that("an unpublished release says so and how to get a published one", {
  msg <- stanli:::download_failure_message(url, "v9.9.9",
    "stanli-runtime-darwin-arm64.tar.gz", "HTTP status was '404 Not Found'",
    release_found = FALSE, paired = TRUE)
  expect_match(msg, "v9.9.9 is not published", fixed = TRUE)
  expect_match(msg, "pairs with it", fixed = TRUE)
  expect_match(msg, "seantalts/stanli@*release", fixed = TRUE)
  expect_no_match(msg, "build", ignore.case = TRUE)
})

test_that("a release without this platform's runtime names the asset", {
  msg <- stanli:::download_failure_message(url, "v9.9.9",
    "stanli-runtime-darwin-arm64.tar.gz", "HTTP status was '404 Not Found'",
    release_found = TRUE)
  expect_match(msg, "has no stanli-runtime-darwin-arm64.tar.gz", fixed = TRUE)
  expect_match(msg, "STANLI_RUNTIME", fixed = TRUE)
})

test_that("any other failure keeps the reason and the URL", {
  msg <- stanli:::download_failure_message(url, "v9.9.9",
    "stanli-runtime-darwin-arm64.tar.gz", "Could not resolve host",
    release_found = NA)
  expect_match(msg, url, fixed = TRUE)
  expect_match(msg, "Could not resolve host", fixed = TRUE)
})

test_that("an unpublished release asked for by name is not called the pair", {
  msg <- stanli:::download_failure_message(url, "v9.9.9",
    "stanli-runtime-darwin-arm64.tar.gz", "HTTP status was '404 Not Found'",
    release_found = FALSE, paired = FALSE)
  expect_match(msg, "v9.9.9 is not published", fixed = TRUE)
  expect_no_match(msg, "pairs with", fixed = TRUE)
})
