data {
  int<lower=1> N;
  int<lower=1> J;
  int<lower=1> K;
  array[N] int<lower=1, upper=J> subject;
  matrix[N, K] X;
  vector[N] y;
}
parameters {
  matrix[K, J] z_raw;
  vector<lower=0>[K] tau;
  vector[K] beta;
  real<lower=0> sigma;
}
transformed parameters {
  matrix[K, J] z = diag_pre_multiply(tau, z_raw);
  vector[N] mu;
  for (i in 1 : N) {
    mu[i] = X[i] * (beta + z[ : , subject[i]]);
  }
}
model {
  to_vector(z_raw) ~ std_normal();
  tau ~ cauchy(0, 1);
  beta ~ std_normal();
  sigma ~ cauchy(0, 1);
  y ~ student_t(4, mu, sigma);
}
