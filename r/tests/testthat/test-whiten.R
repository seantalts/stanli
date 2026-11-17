skip_without_whiten <- function() {
  skip_without_runtime()
  skip_if_not_installed("Matrix")
  ptr <- stanli_model(code = "parameters { real x; } model { x ~ normal(0, 1); }")
  if (inherits(try(parameter_annotations(ptr), silent = TRUE), "try-error"))
    skip("runtime has no Laplace whitening")
}

es_whiten_model <- function() {
  stanli_model(code = "
    data { int<lower=0> J; vector[J] y; vector<lower=0>[J] sigma; }
    parameters { real mu; real<lower=0> tau; vector[J] theta_tilde; }
    transformed parameters { vector[J] theta = mu + tau * theta_tilde; }
    model {
      mu ~ normal(0, 5); tau ~ cauchy(0, 5);
      theta_tilde ~ normal(0, 1); y ~ normal(theta, sigma);
    }
    generated quantities {
      real tau_sq = square(tau);
      vector[J] y_rep;
      for (j in 1:J) y_rep[j] = normal_rng(theta[j], sigma[j]);
    }", data = list(J = 8L, y = c(28, 8, -3, 7, -1, 1, 18, 12),
                    sigma = c(15, 10, 16, 11, 9, 11, 10, 18)))
}

whiten_fixture <- function(inner, seed) {
  set.seed(seed)
  n <- inner$n_unconstrained
  lower <- matrix(0, n, n)
  lower[lower.tri(lower)] <- rnorm(n * (n - 1) / 2) * 0.3 *
    (runif(n * (n - 1) / 2) < 0.2)
  diag(lower) <- runif(n, 0.8, 1.6)
  q_hat <- 0.2 * rnorm(n)
  perm <- sample(n)
  fx <- list(inner = inner, n = n, q_hat = q_hat, perm = perm, dense = lower,
             model = whiten(inner, q_hat, perm,
                            Matrix::Matrix(lower, sparse = TRUE)))
  fx$q_of_z <- function(z) {
    q <- q_hat
    q[perm] <- q[perm] + backsolve(t(lower), z, upper.tri = TRUE)
    q
  }
  fx$points <- function(count = 5, scale = 0.4)
    lapply(seq_len(count), function(i) scale * rnorm(n))
  fx
}

draw_at <- function(model, q) {
  fit <- sample_model(model, chains = 1, samples = 1, warmup = 0,
                      fixed_param = TRUE, init = q, refresh = 0)
  setNames(as.vector(fit$draws[1, 1, ]), dimnames(fit$draws)[[3]])
}

test_that("whitening says once per session that it is experimental", {
  skip_without_whiten()
  rm(list = ls(experimental_state), envir = experimental_state)
  inner <- es_whiten_model()
  expect_message(whiten_fixture(inner, 1), "Laplace whitening is experimental")
  expect_no_message(whiten_fixture(inner, 2))
})

test_that("whitened log density differs from the inner one by a constant", {
  skip_without_whiten()
  fx <- whiten_fixture(es_whiten_model(), 1)
  diffs <- vapply(fx$points(), function(z) {
    lp_z <- log_prob_grad(fx$model, z)$lp
    lp_q <- log_prob_grad(fx$inner, fx$q_of_z(z))$lp
    expect_lte(abs(lp_z - lp_q), 1e-10 * max(1, abs(lp_q)))
    lp_z - lp_q
  }, numeric(1))
  expect_lte(diff(range(diffs)), 1e-10 * 10)
})

test_that("the whitened gradient is L^-1 P times the inner gradient", {
  skip_without_whiten()
  fx <- whiten_fixture(es_whiten_model(), 3)
  for (z in fx$points()) {
    g_z <- log_prob_grad(fx$model, z)$grad
    g_q <- log_prob_grad(fx$inner, fx$q_of_z(z))$grad
    want <- forwardsolve(fx$dense, g_q[fx$perm])
    scale <- max(1, abs(want))
    expect_lte(max(abs(g_z - want)), 1e-10 * scale)
    h <- 1e-6
    fd <- vapply(seq_len(fx$n), function(i) {
      e <- numeric(fx$n)
      e[i] <- h
      (log_prob_grad(fx$model, z + e)$lp - log_prob_grad(fx$model, z - e)$lp) /
        (2 * h)
    }, numeric(1))
    expect_lte(max(abs(fd - g_z)), 1e-6 * scale)
  }
})

test_that("output rows match the inner model at q(z)", {
  skip_without_whiten()
  fx <- whiten_fixture(es_whiten_model(), 5)
  expect_identical(fx$model$columns, fx$inner$columns)
  for (z in fx$points(3)) {
    a <- draw_at(fx$model, z)
    b <- draw_at(fx$inner, fx$q_of_z(z))
    expect_identical(names(a), names(b))
    exact <- !startsWith(names(a), "y_rep")
    expect_lte(max(abs(a[exact] - b[exact]) / (abs(b[exact]) + 1e-12)), 1e-10)
    expect_true(all(is.finite(a)))
  }
})

test_that("unconstrain on a whitened model returns z", {
  skip_without_whiten()
  fx <- whiten_fixture(es_whiten_model(), 7)
  n_par <- length(parameter_names(fx$inner, scalars = TRUE))
  for (z in fx$points(3)) {
    row <- draw_at(fx$model, z)[seq_len(n_par)]
    values <- list(mu = row[["mu"]], tau = row[["tau"]],
                   theta_tilde = unname(row[grepl("^theta_tilde", names(row))]))
    expect_equal(unconstrain(fx$model, values), z, tolerance = 1e-9)
    expect_equal(unconstrain(fx$inner, values), fx$q_of_z(z), tolerance = 1e-9)
  }
})

test_that("CSC arrays give the same model as a Matrix", {
  skip_without_whiten()
  fx <- whiten_fixture(es_whiten_model(), 14)
  L <- Matrix::Matrix(fx$dense, sparse = TRUE)
  again <- whiten(fx$inner, fx$q_hat, fx$perm, list(p = L@p, i = L@i, x = L@x))
  z <- fx$points(1)[[1]]
  expect_identical(log_prob_grad(fx$model, z)$lp, log_prob_grad(again, z)$lp)
})

test_that("the inner model still works after whitening", {
  skip_without_whiten()
  inner <- es_whiten_model()
  fx <- whiten_fixture(inner, 13)
  fx$model <- NULL
  gc()
  expect_true(is.finite(log_prob_grad(inner, numeric(inner$n_unconstrained))$lp))
})

test_that("a whitened model keeps its whitening when a run rebuilds it", {
  skip_without_whiten()
  inner <- stanli_model(code = "
    transformed data { real r = normal_rng(0, 1); }
    parameters { vector[3] x; }
    model { x ~ normal(r, 1); }")
  fx <- whiten_fixture(inner, 2)
  z <- fx$points(1)[[1]]
  rebuilt <- stanli:::with_run_seed(fx$model, 99)
  expect_false(identical(rebuilt$ptr, fx$model$ptr))
  expect_false(is.null(rebuilt$whitening))
  inner_rebuilt <- stanli:::with_run_seed(inner, 99)
  expect_equal(log_prob_grad(rebuilt, z)$lp -
                 log_prob_grad(inner_rebuilt, fx$q_of_z(z))$lp,
               log_prob_grad(fx$model, z)$lp -
                 log_prob_grad(inner, fx$q_of_z(z))$lp, tolerance = 1e-10)
  expect_gt(abs(log_prob_grad(rebuilt, z)$lp - log_prob_grad(fx$model, z)$lp),
            1e-6)
})

test_that("invalid factors are refused", {
  skip_without_whiten()
  inner <- es_whiten_model()
  n <- inner$n_unconstrained
  ident <- seq_len(n)
  good <- Matrix::Diagonal(n)
  q_hat <- numeric(n)
  expect_error(whiten(inner, q_hat, ident,
                      Matrix::Matrix(upper.tri(diag(n), TRUE) * 1,
                                     sparse = TRUE)), "lower")
  expect_error(whiten(inner, q_hat[-1], ident, good), "q_hat")
  expect_error(whiten(inner, q_hat, rep(1L, n), good), "permutation")
  expect_error(whiten(inner, q_hat, ident,
                      Matrix::Matrix(diag(c(0, rep(1, n - 1))), sparse = TRUE)),
               "diagonal")
  expect_error(whiten(inner, q_hat, ident, Matrix::Diagonal(n + 1)), "expected")
  expect_error(whiten(fx <- whiten(inner, q_hat, ident, good), q_hat, ident,
                      good), "already whitened")
})

test_that("parameter_annotations lists annotated parameters", {
  skip_without_whiten()
  m <- suppressWarnings(stanli_model(code = "
    parameters { real mu; @latent vector[3] a; real<lower=0> s; }
    model { a ~ normal(mu, s); }"))
  expect_identical(parameter_annotations(m), list(a = "latent"))
  expect_identical(parameter_annotations(es_whiten_model()), setNames(list(), character(0)))
})

test_that("the whitening routines leave R's protect stack balanced", {
  skip_without_whiten()
  inner <- es_whiten_model()
  n <- inner$n_unconstrained
  A <- Matrix::sparseMatrix(i = c(1:n, 2:n), j = c(1:n, 1:(n - 1)),
                            x = c(rep(2, n), rep(-0.5, n - 1)), dims = c(n, n))
  U <- matrix(c(1, rep(0, n - 1)), n, 1)
  noise <- capture.output(type = "message", {
    for (i in 1:3) {
      f <- stanli:::lw_ldlt(A)
      stanli:::lw_lowrank(f, U)
      whitened <- whiten(inner, numeric(n), seq_len(n), Matrix::Diagonal(n))
      parameter_annotations(inner)
      parameter_annotations(whitened)
      stanli:::lw_latent_index(inner, "mu")
    }
  })
  expect_false(any(grepl("imbalance", noise)), info = paste(noise, collapse = "\n"))
})

CENTRED_STAN <- "
  data { int<lower=1> J; int<lower=1> N; array[N] int<lower=1, upper=J> g;
         vector[N] y; }
  parameters {
    real mu; real<lower=0> sigma_a;
    @latent vector[J] a;
    real<lower=0> sigma_y;
  }
  model {
    sigma_a ~ normal(0, 1); sigma_y ~ normal(0, 1); mu ~ normal(0, 5);
    a ~ normal(mu, sigma_a);
    y ~ normal(a[g], sigma_y);
  }"

centred_model <- function(J = 12L, N = 120L) {
  set.seed(5)
  g <- c(seq_len(J), sample(J, N - J, replace = TRUE))
  a <- 1.3 + 0.4 * rnorm(J)
  suppressWarnings(stanli_model(code = CENTRED_STAN, data = list(
    J = J, N = N, g = g, y = a[g] + 0.7 * rnorm(N))))
}

test_that("laplace_whiten finds the latent block and whitens around the mode", {
  skip_without_whiten()
  m <- centred_model()
  out <- laplace_whiten(m, seed = 3)
  r <- out$report
  expect_identical(r$n, 15L)
  expect_identical(r$n_latent, 12L)
  expect_identical(r$n_theta, 3L)
  expect_identical(r$latent_index, 3:14)
  expect_setequal(r$perm, seq_len(15))
  expect_identical(r$n_clipped, 0L)
  expect_equal(r$gradients$total, sum(unlist(r$gradients[-4])))
  expect_lt(abs(r$q_hat[2]) , 10)
  z <- numeric(15)
  expect_true(is.finite(log_prob_grad(out$model, z)$lp))
  expect_equal(log_prob_grad(out$model, z)$lp,
               log_prob_grad(m, r$q_hat)$lp - 0 * 1, tolerance = 1e-9)
  expect_identical(out$model$columns, m$columns)
})

test_that("a latent argument wins over annotations and bad names are refused", {
  skip_without_whiten()
  m <- centred_model()
  same <- laplace_whiten(m, latent = "a", seed = 3)
  expect_identical(same$report$latent_index, 3:14)
  expect_error(laplace_whiten(m, latent = "nope"), "not a parameter")
  expect_error(laplace_whiten(m, latent = c("a", "a")), "twice")
  plain <- stanli_model(code = "parameters { real x; } model { x ~ normal(0, 1); }")
  expect_error(laplace_whiten(plain), "@latent")
})

test_that("the gradient probe finds the dense Hessian's pattern", {
  skip_without_whiten()
  code <- "data { int<lower=1> K; vector[K] y; }
    parameters { real<lower=0> s; real<lower=0> sy; @latent vector[K] x; }
    model { s ~ normal(0, 1); sy ~ normal(0, 1); x[1] ~ normal(0, 3);
      x[2:K] ~ normal(x[1:(K - 1)], s); y ~ normal(x, sy); }"
  set.seed(2)
  for (K in c(40L, 600L)) {
    m <- suppressWarnings(stanli_model(code = code, data = list(
      K = K, y = cumsum(rnorm(K, 0, 0.3)))))
    iu <- stanli:::lw_latent_index(m, "x")
    q <- 0.3 * rnorm(m$n_unconstrained)
    counter <- new.env()
    counter$n <- 0L
    P <- stanli:::lw_probe_pattern(m, q, iu, 1e-6, counter, 1)
    dense <- stanli:::lw_pattern(log_prob_hessian(m, q)$hessian, 1e-6)[iu, iu]
    expect_identical(as.matrix(P) != 0, as.matrix(dense) != 0)
    expect_lt(counter$n, 1 + 2 * m$n_unconstrained)
    step <- 1e-4 * pmax(1, abs(q[iu]))
    H <- Matrix::Matrix(log_prob_hessian(m, q)$hessian[iu, iu], sparse = TRUE)
    grad <- function(x) log_prob_grad(m, x)
    expect_true(stanli:::lw_probe_agrees(grad, q, iu, H, step, 1e-6))
    expect_false(stanli:::lw_probe_agrees(grad, q, iu, Matrix::Diagonal(x = Matrix::diag(H)),
                                          step, 1e-6))
  }
})

test_that("probe and dense patterns give the same whitening", {
  skip_without_whiten()
  m <- centred_model()
  a <- laplace_whiten(m, seed = 3)$report
  b <- laplace_whiten(m, seed = 3, pattern = "dense")$report
  expect_equal(a$q_hat, b$q_hat, tolerance = 1e-8)
  expect_identical(a$nnz_L, b$nnz_L)
  expect_lt(a$gradients$pattern_hessian, b$gradients$pattern_hessian)
  expect_identical(b$gradients$pattern_hessian, 1L + 2L * 15L)
})

test_that("the eigenpair search finds a negative eigenvalue among many positive ones", {
  skip_without_whiten()
  d <- seq(1, 400, length.out = 300)
  d[150] <- -80
  Q <- Matrix::Diagonal(x = d)
  r <- stanli:::lw_bad_eigenpairs(Q, 1e-8, 1L)
  expect_equal(r$w, -80)
  expect_equal(abs(as.numeric(r$V)), as.numeric(seq_len(300) == 150))
})

test_that("a whitened hierarchical model samples to the default posterior", {
  skip_without_whiten()
  m <- centred_model()
  out <- laplace_whiten(m, seed = 3)
  reference <- sample_model(m, chains = 4, seed = 21, warmup = 1000,
                            samples = 1000, refresh = 0)
  fit <- sample_model(out$model, chains = 4, seed = 22, warmup = 200,
                      samples = 1000, metric = "unit_e", adapt_metric = FALSE,
                      refresh = 0)
  expect_identical(dimnames(fit$draws)[[3]], dimnames(reference$draws)[[3]])
  cols <- seq_len(length(m$columns))
  mean_of <- function(f, j) mean(f$draws[, , j])
  sd_of <- function(f, j) sd(as.vector(f$draws[, , j]))
  gap <- vapply(cols, function(j)
    abs(mean_of(fit, j) - mean_of(reference, j)) / sd_of(reference, j),
    numeric(1))
  expect_lt(max(gap), 0.15)
})
