test_that("every routine the R code calls is registered by the compiled code", {
  expect_identical(stanli:::missing_native_routines(), character())
})

test_that("compiled code from another install is reported, not left cryptic", {
  expect_error(
    stanli:::check_native_routines(registered = "stanli_bridge_loaded"),
    "different installs.*restart R")
})
