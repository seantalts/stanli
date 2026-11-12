hessian_model <- function() {
  stanli_model(code = "
    parameters { vector[2] x; real<lower=0> s; }
    model {
      x ~ multi_normal([1, -2]', [[1, 0.8], [0.8, 1]]);
      s ~ lognormal(0, 0.5);
    }")
}

test_that("log_prob_hessian returns lp, gradient and a 3 x 3 matrix", {
  skip_without_runtime()
  m <- hessian_model()
  q <- c(1.7, -2.3, 0.6)
  out <- log_prob_hessian(m, q)
  ref <- log_prob_grad(m, q)
  expect_identical(out$lp, ref$lp)
  expect_identical(out$grad, ref$grad)
  expect_identical(dim(out$hessian), c(3L, 3L))
  expect_lt(abs(out$hessian[3, 3] + 4), 1e-8)
  expect_lt(abs(out$hessian[1, 3]), 1e-8)
  expect_identical(out$hessian, t(out$hessian))
})

test_that("log_prob_hessian checks its arguments and reports rejections", {
  skip_without_runtime()
  m <- hessian_model()
  expect_error(log_prob_hessian(m, c(0, 0)), "unconstrained")
  bad <- stanli_model(code = "
    parameters { real<lower=0> s; real t; }
    model { t ~ normal(0, s); s ~ lognormal(0, 1); }")
  expect_error(log_prob_hessian(bad, c(-800, 0)), "Hessian")
})
