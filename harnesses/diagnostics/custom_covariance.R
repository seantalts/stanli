# R interface smoke probe for #656; this is not the reporter's model.
# Run from the repository root with a matching R package and STANLI_RUNTIME.
library(stanli)
cat("R ", as.character(getRversion()), "\nstanli ",
    as.character(packageVersion("stanli")), "\nruntime ", stanli_build_id(),
    "\n", sep = "")
data_for <- function(n, invalid = 0L) {
  # Explicit dimension preserves vector shape at N=1 in Stan JSON.
  list(N = as.integer(n), x = array(seq.int(0, n - 1) / max(1, n - 1), n),
       y = array(sin(seq.int(0, n - 1)), n), invalid = as.integer(invalid))
}
for (n in c(1L, 3L, 40L)) {
  m <- stanli_model(file = "harnesses/diagnostics/custom_covariance.stan",
                    data = data_for(n))
  v <- log_prob_grad(m, rep(0, m$n_unconstrained))
  stopifnot(is.finite(v$lp), all(is.finite(v$grad)))
  cat("valid N=", n, " lp=", format(v$lp, digits = 17),
      " gradient_length=", length(v$grad), "\n", sep = "")
}
for (bad in 1:2) {
  m <- stanli_model(file = "harnesses/diagnostics/custom_covariance.stan",
                    data = data_for(3L, bad))
  e <- tryCatch({log_prob_grad(m, rep(0, m$n_unconstrained)); NA_character_},
                error = function(e) conditionMessage(e))
  stopifnot(!is.na(e), grepl("cholesky_decompose", e))
  cat("invalid=", bad, " error=", e, "\n", sep = "")
}
m <- stanli_model(file = "tests/rethinking/ch14_m14_8nc.stan",
                  data = "tests/rethinking/ch14_m14_8nc.json")
v <- log_prob_grad(m, rep(0, m$n_unconstrained))
stopifnot(is.finite(v$lp), all(is.finite(v$grad)))
cat("rethinking ch14_m14_8nc lp=", format(v$lp, digits = 17),
    " gradient_length=", length(v$grad), "\n", sep = "")
m <- stanli_model(file = "harnesses/diagnostics/custom_covariance.stan",
                  data = data_for(3L))
fit <- sample_model(m, chains = 1, parallel_chains = 1, warmup = 25,
                    samples = 25, seed = 1, refresh = 0)
stopifnot(inherits(fit, "stanli_fit"))
cat("R sampling smoke completed: 25 warmup + 25 retained draws\n")
