data { int<lower=1> N; vector[N] x; vector[N] bhaz; vector[N] cbhaz;
       array[N] int<lower=0, upper=1> cens; }
parameters { real a; real b; }
model {
  vector[N] log_mu = a + x * b;
  a ~ normal(0, 1); b ~ normal(0, 1);
  for (n in 1:N) {
    if (cens[n] == 0)
      target += log(bhaz[n]) + log_mu[n] - cbhaz[n] * exp(log_mu[n]);
    else
      target += -cbhaz[n] * exp(log_mu[n]);
  }
}
