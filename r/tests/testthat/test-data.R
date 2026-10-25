# The data writer, which has no runtime dependency.

test_that("non-finite reals go out in the spellings Stan's reader accepts", {
  expect_equal(stanli:::json_scalar(Inf), "\"Infinity\"")
  expect_equal(stanli:::json_scalar(-Inf), "\"-Infinity\"")
  expect_equal(stanli:::json_scalar(NaN), "\"NaN\"")
  expect_equal(stanli:::json_scalar(NA_real_), "\"NaN\"")
  expect_equal(stanli:::to_json(list(lb = -Inf, ub = 3.5)),
               "{\"lb\": \"-Infinity\", \"ub\": 3.5}")
  expect_equal(stanli:::to_json(list(y = c(1.5, Inf, NA))),
               "{\"y\": [1.5, \"Infinity\", \"NaN\"]}")
  expect_equal(stanli:::to_json(list(M = matrix(c(1, Inf, 2, 3), 2, 2))),
               "{\"M\": [[1, 2], [\"Infinity\", 3]]}")
})

test_that("integer data still rejects a missing value", {
  expect_error(stanli:::json_scalar(NA_integer_), "integer data")
  expect_error(stanli:::json_scalar(NA), "integer data")
  expect_error(stanli:::to_json(list(N = NA_integer_)), "integer data")
})

test_that("a model reads data holding an infinite bound", {
  if (!stanli_available()) skip("no stanli runtime installed")
  m <- stanli_model(code = "
    data { real lb; real<upper=5> ub; }
    parameters { real mu; }
    model { mu ~ normal(0, 1); target += ub; }",
    data = list(lb = -Inf, ub = 3.5))
  expect_s3_class(m, "stanli_model")
  expect_true(is.finite(log_prob_grad(m, 0)$lp))
})

test_that("arrays of any rank go out as nested arrays, first index outermost", {
  a3 <- array(1:24, c(2, 3, 4))
  json <- stanli:::to_json(list(a = a3))
  expect_equal(json, paste0(
    "{\"a\": [[[1, 7, 13, 19], [3, 9, 15, 21], [5, 11, 17, 23]], ",
    "[[2, 8, 14, 20], [4, 10, 16, 22], [6, 12, 18, 24]]]}"))
  parsed <- jsonlite_free_parse(json)
  expect_equal(parsed, list(a = a3))
  a4 <- array(seq_len(2 * 3 * 2 * 2) + 0.5, c(2, 3, 2, 2))
  expect_equal(jsonlite_free_parse(stanli:::to_json(list(a = a4)))$a, a4)
  expect_equal(stanli:::to_json(list(v = array(1:3, 3), w = array(7, 1))),
               "{\"v\": [1, 2, 3], \"w\": [7]}")
})

test_that("an unnamed list is a JSON array of its elements", {
  ms <- list(matrix(1:6, 2, 3), matrix(7:12, 2, 3))
  expect_equal(stanli:::to_json(list(ms = ms)),
               "{\"ms\": [[[1, 3, 5], [2, 4, 6]], [[7, 9, 11], [8, 10, 12]]]}")
  expect_equal(stanli:::to_json(list(vs = list(c(1, 2, 3), c(4, 5, 6)))),
               "{\"vs\": [[1, 2, 3], [4, 5, 6]]}")
  expect_equal(stanli:::to_json(list(n = list(list(1, 2), list(3)))),
               "{\"n\": [[1, 2], [3]]}")
  expect_equal(stanli:::to_json(list(x = list())), "{\"x\": []}")
  expect_equal(stanli:::to_json(list()), "{}")
  expect_equal(stanli:::to_json(list(N = 3L, y = c(1, 2))),
               "{\"N\": 3, \"y\": [1, 2]}")
})

test_that("zero-length extents keep the extents around them", {
  expect_equal(stanli:::to_json(list(z = array(0, c(2, 0)))),
               "{\"z\": [[], []]}")
  expect_equal(stanli:::to_json(list(z = array(0, c(0, 3)))), "{\"z\": []}")
  expect_equal(stanli:::to_json(list(z = array(0, c(2, 0, 3)))),
               "{\"z\": [[], []]}")
  expect_equal(stanli:::to_json(list(z = matrix(0, 0, 3))), "{\"z\": []}")
  expect_equal(stanli:::to_json(list(z = matrix(0, 3, 0))),
               "{\"z\": [[], [], []]}")
  expect_equal(stanli:::to_json(list(z = numeric(0))), "{\"z\": []}")
})

test_that("a mixed named and unnamed list is refused", {
  expect_error(stanli:::to_json(list(a = 1, 2)), "names")
})

data_echo_model <- function(data) {
  stanli_model(code = "
    data {
      array[2, 3, 4] real a3;
      array[2, 3, 2, 2] int a4;
      array[2] matrix[2, 3] ms;
      array[2] vector[3] vs;
      array[2] row_vector[3] rs;
      array[2, 2] vector[3] nested;
    }
    generated quantities {
      array[2, 3, 4] real a3_out = a3;
      array[2, 3, 2, 2] int a4_out = a4;
      array[2] matrix[2, 3] ms_out = ms;
      array[2] vector[3] vs_out = vs;
      array[2] row_vector[3] rs_out = rs;
      array[2, 2] vector[3] nested_out = nested;
    }", data = data)
}

echo_draw <- function(model, name) {
  fit <- sample_model(model, fixed_param = TRUE, chains = 1, samples = 1,
                      refresh = 0)
  columns <- grep(paste0("^", name, "\\["), fit$columns)
  unname(fit$draws[1, 1, columns])
}

test_that("a model reads back arrays of rank three and four, and lists", {
  skip_without_runtime()
  a3 <- array(seq_len(24) / 4, c(2, 3, 4))
  a4 <- array(seq_len(24), c(2, 3, 2, 2))
  ms <- list(matrix(seq_len(6), 2, 3), matrix(seq_len(6) + 10, 2, 3))
  vs <- list(c(1, 2, 3), c(4, 5, 6))
  rs <- list(c(7, 8, 9), c(10, 11, 12))
  nested <- list(list(c(1, 2, 3), c(4, 5, 6)), list(c(7, 8, 9), c(10, 11, 12)))
  m <- data_echo_model(list(a3 = a3, a4 = a4, ms = ms, vs = vs, rs = rs,
                            nested = nested))
  expect_equal(echo_draw(m, "a3_out"), as.vector(a3))
  expect_equal(echo_draw(m, "a4_out"), as.vector(a4))
  ms_expected <- array(0, c(2, 2, 3))
  for (k in 1:2) ms_expected[k, , ] <- ms[[k]]
  expect_equal(echo_draw(m, "ms_out"), as.vector(ms_expected))
  expect_equal(echo_draw(m, "vs_out"), as.vector(do.call(rbind, vs)))
  expect_equal(echo_draw(m, "rs_out"), as.vector(do.call(rbind, rs)))
  nested_expected <- array(0, c(2, 2, 3))
  for (i in 1:2) for (j in 1:2) nested_expected[i, j, ] <- nested[[i]][[j]]
  expect_equal(echo_draw(m, "nested_out"), as.vector(nested_expected))
})

test_that("a model reads zero-extent containers", {
  skip_without_runtime()
  m <- stanli_model(code = "
    data {
      array[2, 0] real z;
      array[0] vector[3] zv;
      array[0, 4] int zi;
      matrix[0, 3] zm;
      matrix[3, 0] zn;
      array[0] matrix[2, 2] zms;
    }
    generated quantities {
      array[6] int sizes = {size(z), size(zv), size(zi), rows(zm), cols(zn),
                            size(zms)};
    }", data = list(z = array(0, c(2, 0)), zv = list(),
                    zi = array(0L, c(0, 4)), zm = matrix(0, 0, 3),
                    zn = matrix(0, 3, 0), zms = list()))
  fit <- sample_model(m, fixed_param = TRUE, chains = 1, samples = 1,
                      refresh = 0)
  expect_equal(unname(fit$draws[1, 1, ]), c(2, 0, 0, 0, 0, 0))
})
