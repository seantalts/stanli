#!/usr/bin/env Rscript
# These shapes cannot be recovered from length(x) alone.
source("tools/stan_data_json.R")
code <- "data {
  int<lower=0> N;
  real<lower=0,upper=1> p;
  array[1] int<lower=0> y;
  vector[1] v;
  matrix[1,2] M;
  array[2,1] real z;
}"
stopifnot(identical(decl_ndim(code), list(N = 0, p = 0, y = 1, v = 1,
                                        M = 2, z = 2)))
out <- tempfile()
write_json(list(N = 1L, p = 0.5, y = 3L, v = 7,
                M = matrix(c(2, 4), 1), z = matrix(c(5, 6), 2)), code, out)
stopifnot(identical(readLines(out),
  '{"N":1,"p":5e-01,"y":[3],"v":[7],"M":[[2,4]],"z":[[5],[6]]}'))
stopifnot(identical(emit(matrix(1:6, nrow = 2), 2), '[[1,3,5],[2,4,6]]'),
          identical(emit(array(1:8, dim = c(2, 1, 2, 2)), 4),
                    '[[[[1,5],[3,7]]],[[[2,6],[4,8]]]]'),
          identical(num(NA_real_), "NaN"), identical(num(Inf), "Infinity"))
unlink(out)
cat("Stan JSON: constrained scalars, length-one vectors, arrays, matrices and non-finite values pass.\n")
