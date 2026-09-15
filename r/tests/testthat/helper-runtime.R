# These need a runtime. Point STANLI_RUNTIME at a build (and
# STANLI_STANC at a stanc3 if that build does not embed one) to run them;
# they skip otherwise, so R CMD check passes on a machine with neither.
skip_without_runtime <- function() {
  if (!stanli_available()) skip("no stanli runtime installed")
}

es_model <- function() {
  code <- "
    data { int<lower=0> J; array[J] real y; array[J] real<lower=0> sigma; }
    parameters { real mu; real<lower=0> tau; vector[J] theta_tilde; }
    transformed parameters { vector[J] theta = mu + tau * theta_tilde; }
    model {
      mu ~ normal(0, 5); tau ~ cauchy(0, 5);
      theta_tilde ~ std_normal(); y ~ normal(theta, sigma);
    }"
  stanli_model(code = code, data = list(
    J = 8L, y = c(28, 8, -3, 7, -1, 1, 18, 12),
    sigma = c(15, 10, 16, 11, 9, 11, 10, 18)))
}

progress_model <- function() {
  stanli_model(code = "
    parameters { real x; }
    model { x ~ normal(0, 1); }")
}


log_lik_model <- function(prior_sd = 1) {
  stanli_model(code = "
    data { int N; vector[N] y; real prior_sd; }
    parameters { real mu; }
    model { mu ~ normal(0, prior_sd); y ~ normal(mu, 1); }
    generated quantities {
      vector[N] log_lik;
      vector[N] alternate;
      vector[N] y_rep;
      real log_likelihood = 42;
      real scalar_ll = normal_lpdf(y[1] | mu, 1);
      for (n in 1:N) {
        log_lik[n] = normal_lpdf(y[n] | mu, 1);
        alternate[n] = log_lik[n];
        y_rep[n] = normal_rng(mu, 1);
      }
    }", data = list(N = 12L, y = seq(-1, 1, length.out = 12),
                     prior_sd = prior_sd))
}

