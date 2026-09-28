functions {
  vector selected_rng(data int family, vector p, matrix L) {
    vector[rows(p)] result;
    if (family == 0) result = rep_vector(categorical_rng(p), rows(p));
    else if (family == 1) result = rep_vector(categorical_logit_rng(p), rows(p));
    else if (family == 2) result = rep_vector(poisson_binomial_rng(p), rows(p));
    else result = multi_normal_cholesky_rng(p, L);
    return result;
  }
}
data {
  int<lower=0, upper=3> family;
  int<lower=0, upper=1> region;
  int<lower=0> N;
}
parameters { vector[N] p; matrix[N,N] L; }
model { p ~ normal(0, 1); to_vector(L) ~ normal(0, 1); }
generated quantities {
  real before = normal_rng(0, 1);
  vector[N] draw = rep_vector(0, N);
  if (region) {
    int i = 0;
    while (i < 2) {
      if (family == 0) draw = rep_vector(categorical_rng(p), N);
      else if (family == 1) draw = rep_vector(categorical_logit_rng(p), N);
      else if (family == 2) draw = rep_vector(poisson_binomial_rng(p), N);
      else draw = multi_normal_cholesky_rng(p, L);
      i += 1;
    }
  } else draw = selected_rng(family, p, L);
  real after = normal_rng(0, 1);
}
