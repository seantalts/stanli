test_that("multinomial posterior predictions build and sample from R (#660)", {
  skip_without_runtime()
  path <- tempfile(fileext = ".stan")
  on.exit(unlink(path), add = TRUE)
  writeLines("
    data {
      int<lower=2> K;
      array[K] int<lower=0> y;
      vector<lower=0>[K] alpha;
    }
    parameters { simplex[K] theta; }
    model {
      theta ~ dirichlet(alpha);
      y ~ multinomial(theta);
    }
    generated quantities {
      array[K] int y_rep = multinomial_rng(theta, sum(y));
    }", path)

  for (K in c(2L, 4L)) {
    for (total in c(0L, 200L)) {
      y <- c(total, rep(0L, K - 1L))
      m <- stanli_model(file = path, data = list(
        K = K, y = y, alpha = rep(1, K)), allow_interpreter = FALSE)
      theta_names <- paste0("theta[", seq_len(K), "]")
      rep_names <- paste0("y_rep[", seq_len(K), "]")
      expect_identical(m$columns, c(theta_names, rep_names))
      expect_equal(m$n_unconstrained, K - 1L)
      theta <- seq_len(K) / sum(seq_len(K))
      q <- unconstrain(m, list(theta = theta))
      lp <- log_prob_grad(m, q)
      expect_true(is.finite(lp$lp))
      expect_true(all(is.finite(lp$grad)))

      draw <- function() sample_model(
        m, fixed_param = TRUE, chains = 2, seed = 19, samples = 8,
        init = q, refresh = 0)
      fit <- draw()
      expect_identical(dimnames(fit$draws)[[3]], m$columns)
      for (chain in 1:2) {
        counts <- fit$draws[, chain, rep_names]
        expect_true(all(counts >= 0 & counts == floor(counts)))
        expect_equal(unname(rowSums(counts)), rep(total, 8))
      }
      expect_identical(draw()$draws, fit$draws)
    }
  }
})
