# The data writer, which has no runtime dependency.

echo_draw <- function(model, name) {
  fit <- sample_model(model, fixed_param = TRUE, chains = 1, samples = 1,
                      refresh = 0)
  columns <- grep(paste0("^", name, "\\["), fit$columns)
  unname(fit$draws[1, 1, columns])
}

test_that("non-finite reals go out in the spellings Stan's reader accepts", {
  expect_equal(stanli:::json_scalar(Inf), "\"Infinity\"")
  expect_equal(stanli:::json_scalar(-Inf), "\"-Infinity\"")
  expect_equal(stanli:::json_scalar(NaN), "\"NaN\"")
  expect_equal(stanli:::to_json(list(lb = -Inf, ub = 3.5)),
               "{\"lb\": \"-Infinity\", \"ub\": 3.5}")
  expect_equal(stanli:::to_json(list(y = c(1.5, Inf, NaN))),
               "{\"y\": [1.5, \"Infinity\", \"NaN\"]}")
  expect_equal(stanli:::to_json(list(M = matrix(c(1, Inf, 2, 3), 2, 2))),
               "{\"M\": [[1, 2], [\"Infinity\", 3]]}")
})

test_that("a missing value is an error naming the variable, as in cmdstanr", {
  for (value in list(NA, NA_integer_, NA_real_, c(1, NA, Inf),
                     list(1, NA), matrix(c(1, NA), 1, 2)))
    expect_error(stanli:::to_json(list(y = value)),
                 "Variable 'y' has NA values.", fixed = TRUE)
  expect_error(stanli:::to_json(list(t = list("1" = 1, "2" = NA_real_))),
               "Variable 't' has NA values.", fixed = TRUE)
})

test_that("factors, data frames and named lists go out as cmdstanr writes them", {
  expect_equal(stanli:::to_json(list(i = factor(5, levels = c(5, 7)))),
               "{\"i\": 1}")
  expect_equal(stanli:::to_json(list(i = factor(c("a", "b", "a")))),
               "{\"i\": [1, 2, 1]}")
  expect_equal(stanli:::to_json(list(m = data.frame(a = 1:3, b = 4:6))),
               "{\"m\": [[1, 4], [2, 5], [3, 6]]}")
  expect_equal(stanli:::to_json(list(av = split(1:6, rep(c("a", "b"), each = 3)))),
               "{\"av\": [[1, 2, 3], [4, 5, 6]]}")
  # A list named "1", "2", ... is a tuple.
  expect_equal(stanli:::to_json(list(t = list("1" = 1, "2" = 2))),
               "{\"t\": {\"1\": 1, \"2\": 2}}")
})

test_that("a model reads factors and data frames as cmdstanr does", {
  skip_without_runtime()
  m <- stanli_model(code = "
    data { int i; array[3] int ai; matrix[3, 2] m; array[2] vector[3] av; }
    generated quantities {
      array[11] real out = {i, ai[1], ai[2], m[1, 2], m[3, 1], av[1][1],
        av[2][3], m[2, 2], ai[3], av[1][3], av[2][1]};
    }",
    data = list(i = factor(5, levels = c(5, 7)), ai = factor(c(10, 20, 10)),
                m = data.frame(a = 1:3, b = 4:6),
                av = split(c(1, 2, 3, 4, 5, 6), rep(c("a", "b"), each = 3))))
  expect_equal(echo_draw(m, "out"), c(1, 1, 2, 4, 3, 1, 6, 5, 1, 3, 4))
})

test_that("a model reads data holding an infinite bound", {
  skip_without_runtime()
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

test_that("data whose dims differ only by extents of 1 is read in order", {
  skip_without_runtime()
  x <- c(3, 1, 2)
  m <- stanli_model(code = "
    data { vector[1] v1; row_vector[1] rv1; array[1] real a1;
      array[3] vector[1] av; array[1] vector[1] av1;
      vector[3] sx; array[3] real col; array[3] real row;
      matrix[1, 1] m11; real s; int n;
      array[2] tuple(vector[1], real) tv; }
    generated quantities {
      array[16] real out = {v1[1], rv1[1], a1[1], av[1][1], av[2][1],
        av[3][1], av1[1][1], sx[1], sx[3], col[2], row[3], m11[1, 1], s, n,
        tv[2].1[1], tv[2].2};
    }",
    data = list(v1 = matrix(0.5, 1, 1), rv1 = matrix(1.5, 1, 1),
                a1 = matrix(2.5, 1, 1), av = list(1, 2, 3), av1 = list(0.25),
                sx = scale(x), col = matrix(c(4, 5, 6), 3, 1),
                row = matrix(c(7, 8, 9), 1, 3), m11 = array(3.5, 1),
                s = array(4.5, 1), n = array(6L, c(1, 1, 1)),
                tv = list(list("1" = 0.1, "2" = 0.2),
                          list("1" = 0.3, "2" = 0.4))))
  expect_equal(echo_draw(m, "out"),
               c(0.5, 1.5, 2.5, 1, 2, 3, 0.25, as.vector(scale(x))[c(1, 3)],
                 5, 9, 3.5, 4.5, 6, 0.3, 0.4))
  expect_error(stanli_model(code = "data { matrix[2, 3] M; }",
                            data = list(M = matrix(1:6, 3, 2))),
               "dims declared=(2,3); dims found=(3,2)", fixed = TRUE)
  expect_error(stanli_model(code = "data { matrix[2, 3] M; }",
                            data = list(M = 1:6)),
               "dims declared=(2,3); dims found=(6)", fixed = TRUE)
})

test_that("starting values whose dims differ only by extents of 1 are read in order", {
  skip_without_runtime()
  m <- stanli_model(code = "
    parameters { vector[1] v; array[1] vector[1] av; array[3] vector[1] a3;
      vector[3] c; matrix[1, 1] m; real s; }
    model { s ~ normal(0, 1); }")
  expect_equal(unconstrain(m, list(v = matrix(0.3, 1, 1), av = list(0.4),
                                   a3 = list(1, 2, 3),
                                   c = matrix(c(4, 5, 6), 3, 1),
                                   m = array(7, 1), s = array(8, 1))),
               c(0.3, 0.4, 1, 2, 3, 4, 5, 6, 7, 8))
  expect_equal(unconstrain(m, list(v = matrix(0.3, 1, 1)), partial = TRUE,
                           seed = 1)[1], 0.3)
  expect_equal(unconstrain(m, list(av = list(0.4)), partial = TRUE,
                           seed = 1)[2], 0.4)
  L <- stanli_model(code = "parameters { cholesky_factor_corr[2] L; }")
  expect_error(unconstrain(L, list(L = c(1, 0.6, 0, 0.8))),
               "dims declared=(2,2); dims found=(4)", fixed = TRUE)
})

test_that("a length-1 R vector fills a declaration whose extents are all 1", {
  skip_without_runtime()
  # Issue #671: R writes seasons = 0 as a JSON scalar.
  expect_s3_class(stanli_model(
    code = "data { int<lower=1> K; array[K] real<lower=0> seasons; }
            parameters { real mu; } model { mu ~ normal(0, 1); }",
    data = list(K = 1, seasons = 0)), "stanli_model")
  m <- stanli_model(code = "
    data { int<lower=1> K; array[K] real<lower=0> seasons; vector[1] v;
      array[1] int k; }
    transformed data { real first = seasons[1]; }
    parameters { vector[1] mu; }
    model { mu ~ normal(0, 1); }
    generated quantities { array[3] real out = {first, v[1], k[1]}; }",
    data = list(K = 1L, seasons = 0.25, v = 0.5, k = 2L))
  fit <- sample_model(m, fixed_param = TRUE, chains = 1, samples = 1,
                      refresh = 0)
  columns <- grep("^out\\[", fit$columns)
  expect_equal(unname(fit$draws[1, 1, columns]), c(0.25, 0.5, 2))
  expect_equal(unconstrain(m, list(mu = 0.75)), 0.75)
  expect_error(stanli_model(code = "data { vector[2] v; }",
                            data = list(v = 0.5)),
               "variable name=v; dims declared=(2); dims found=()",
               fixed = TRUE)
})

test_that("the data is written from its declarations, as cmdstanr 1.0 does", {
  skip_without_runtime()
  code <- "
    data { array[2] vector[3] av; tuple(real, array[2] int) tp;
      array[2, 2] tuple(real, real) w; array[2, 2] tuple(real, real) wn;
      array[2] tuple(vector[1], real) tv; }
    generated quantities {
      array[16] real out = {av[1][3], av[2][1], tp.1, tp.2[2],
        w[1, 2].1, w[2, 1].1, w[2, 2].2, wn[1, 2].1, wn[2, 1].1, wn[2, 2].2,
        tv[1].1[1], tv[2].2, w[1, 1].2, wn[1, 1].2, av[1][1], av[2][3]};
    }"
  # A list with dim holds an array of tuples first index fastest, as in
  # cmdstanr 1.0; nested lists give the same array first index outermost.
  w <- vector("list", 4)
  dim(w) <- c(2, 2)
  for (i in 1:2) for (j in 1:2) w[[i, j]] <- list(10 * i + j, 100 + 10 * i + j)
  wn <- lapply(1:2, function(i) lapply(1:2, function(j)
    list(20 * i + j, 200 + 20 * i + j)))
  m <- stanli_model(code = code, data = list(
    av = split(c(1, 2, 3, 4, 5, 6), rep(1:2, each = 3)),
    tp = list(1.5, c(3L, 4L)), w = w, wn = wn,
    tv = list(list(0.5, 2), list(0.25, 7))))
  expect_equal(echo_draw(m, "out"),
               c(3, 4, 1.5, 4, 12, 21, 122, 22, 41, 242, 0.5, 7, 111, 221,
                 1, 6))
  expect_true(m$source$is_mir)
  expect_identical(m$model_code, code)
})

test_that("a factor for a variable that is not an int is an error, as in cmdstanr", {
  skip_without_runtime()
  expect_error(stanli_model(code = "data { vector[2] x; }",
                            data = list(x = factor(c("a", "b")))),
               "A factor was supplied for 'x', which is declared as 'vector'.",
               fixed = TRUE)
  expect_error(stanli_model(code = "data { tuple(real, int) t; }",
                            data = list(t = 1)),
               "Variable 't' is declared as a tuple and must be a list.",
               fixed = TRUE)
  expect_error(stanli_model(code = "data { tuple(real, int) t; }",
                            data = list(t = list(1))),
               "Variable 't' is a tuple with 2 elements, but 1 were supplied.",
               fixed = TRUE)
  # cmdstanr truncates 2.5 with a warning; CmdStan and stanli reject it.
  expect_error(stanli_model(code = "data { int n; }", data = list(n = 2.5)),
               "int variable contained non-int values")
})

test_that("compiler warnings reach the user once when data is written", {
  skip_without_runtime()
  code <- "data { int n; } transformed data { int k = n / 2; real r = normal_rng(0, 1); } parameters { real mu; } model { mu ~ normal(k + r, 1); }"
  warnings <- character()
  withCallingHandlers(m <- stanli_model(code = code, data = list(n = 4L)),
                      warning = function(w) {
                        warnings <<- c(warnings, conditionMessage(w))
                        invokeRestart("muffleWarning")
                      })
  expect_length(grep("integer division", warnings), 1L)
  # A rebuild under another seed reuses the MIR and warns again for nothing.
  warnings <- character()
  withCallingHandlers(again <- stanli:::with_run_seed(m, 2),
                      warning = function(w) {
                        warnings <<- c(warnings, conditionMessage(w))
                        invokeRestart("muffleWarning")
                      })
  expect_length(warnings, 0L)
  expect_identical(again$seed, 2)
  expect_identical(again$model_code, code)
  expect_true(is.character(bridgestan_data(m)))
})

test_that("complex data goes out as cmdstanr writes it and gets the runtime's message", {
  expect_equal(stanli:::to_json(list(z = 1+2i)), "{\"z\": [1, 2]}")
  expect_equal(stanli:::to_json(list(zv = c(3+4i, 5+6i))),
               "{\"zv\": [[3, 4], [5, 6]]}")
  expect_equal(stanli:::to_json(list(zm = matrix(c(1+2i, 3+4i), 1, 2))),
               "{\"zm\": [[[1, 2], [3, 4]]]}")
  skip_without_runtime()
  for (case in list(
    list("complex z;", list(z = 1+2i)),
    list("complex_vector[2] z;", list(z = c(3+4i, 5+6i))),
    list("array[2] complex_row_vector[3] z;",
         list(z = matrix(complex(real = 1:6, imaginary = 6:1), 2, 3)))))
    expect_error(stanli_model(code = paste("data {", case[[1]],
                                           "} parameters { real mu; } model { mu ~ normal(0, 1); }"),
                              data = case[[2]]),
                 "complex numbers are not supported yet (in variable z)",
                 fixed = TRUE)
})
