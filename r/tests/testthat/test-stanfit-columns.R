test_that("Stan column names recover scalar and rectangular dimensions", {
  parse <- stanli:::stanfit_column_layout
  expected_names <- c("s", "v", "M", "av", "am")
  expected_dims <- list(s = integer(0), v = 12L, M = c(2L, 3L),
                        av = c(2L, 3L), am = c(2L, 2L, 3L))
  columns <- c("s", paste0("v[", 1:12, "]"),
    "M[1,1]", "M[2,1]", "M[1,2]", "M[2,2]", "M[1,3]", "M[2,3]",
    "av[1,1]", "av[2,1]", "av[1,2]", "av[2,2]", "av[1,3]", "av[2,3]",
    "am[1,1,1]", "am[2,1,1]", "am[1,2,1]", "am[2,2,1]",
    "am[1,1,2]", "am[2,1,2]", "am[1,2,2]", "am[2,2,2]",
    "am[1,1,3]", "am[2,1,3]", "am[1,2,3]", "am[2,2,3]")
  expect_identical(parse(columns), list(parameters = expected_names,
                                        dimensions = expected_dims))
  expect_identical(parse(c("one[1]", "lp__"))$dimensions,
                   list(one = 1L, lp__ = integer(0)))
  expect_length(parse(character())$parameters, 0L)
  expect_length(parse(character())$dimensions, 0L)
})

test_that("ambiguous, incomplete, and misordered columns are rejected", {
  parse <- stanli:::stanfit_column_layout
  expect_error(parse(c("x", "x")), "unique")
  expect_error(parse(c("x", NA_character_)), "nonmissing")
  expect_error(parse(c("x", "x[1]")), "mixed scalar")
  expect_error(parse(c("x[1]", "y", "x[2]")), "contiguous")
  expect_error(parse(c("x[1]", "x[3]")), "incomplete rectangular")
  expect_error(parse(c("x[1]", "x[1,2]")), "index dimensions")
  expect_error(parse(c("x[2]", "x[1]")), "Stan column order")
  expect_error(parse(c("x[1,1]", "x[1,2]", "x[2,1]", "x[2,2]")),
               "Stan column order")
  for (bad in c("x[0]", "x[-1]", "x[1.2]", "x[]", "x[1,]", "x.1"))
    expect_error(parse(bad), "invalid Stan column")
  expect_error(parse("x[99999999999999]"), "index dimensions")
})
