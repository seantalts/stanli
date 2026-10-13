data {
  int<lower=1> N;
  int<lower=1> N_1;
  vector[N] Y;
  array[N] int<lower=1, upper=N_1> J_1;
  vector[N] Z_1_1;
  vector[N] Z_1_2;
}
parameters {
  real Intercept;
  real<lower=0> sigma;
  vector<lower=0>[2] sd_1;
  array[2] vector[N_1] z_1;
}
transformed parameters {
  vector[N_1] r_1_1 = sd_1[1] * z_1[1];
  vector[N_1] r_1_2 = sd_1[2] * z_1[2];
}
model {
  vector[N] mu = rep_vector(0.0, N) + Intercept;
  for (n in 1 : N) {
    mu[n] += r_1_1[J_1[n]] * Z_1_1[n] + r_1_2[J_1[n]] * Z_1_2[n];
  }
  for (n in 1 : N) {
    mu[n] += r_1_1[J_1[n]] * Z_1_2[n];
  }
  for (n in 3 : (N - 2)) {
    mu[n] += r_1_2[J_1[n]];
  }
  for (n in 1 : N) {
    mu[n] += 0.5 * r_1_1[J_1[n]];
    mu[n] += Z_1_1[n] * r_1_2[J_1[n]];
  }
  Y ~ normal(mu, sigma);
  Intercept ~ student_t(3, 0, 2.5);
  sigma ~ student_t(3, 0, 2.5);
  sd_1 ~ student_t(3, 0, 2.5);
  z_1[1] ~ std_normal();
  z_1[2] ~ std_normal();
}
