data {
  int<lower=1> N;
  int<lower=1> N_1;
  vector[N] Y;
  array[N] int<lower=1, upper=N_1> J_1;
  array[N] int<lower=1, upper=N_1> J_2;
  array[N] int<lower=1, upper=N_1> J_3;
  vector[N] Z_1;
  vector[N] Z_2;
  vector[N] Z_3;
}
parameters {
  vector[3] Intercept;
  vector<lower=0>[3] sd;
  array[3] vector[N_1] z;
}
transformed parameters {
  vector[N_1] r_1 = sd[1] * z[1];
  vector[N_1] r_2 = sd[2] * z[2];
  vector[N_1] r_3 = sd[3] * z[3];
}
model {
  vector[N] mu2 = rep_vector(0.0, N);
  vector[N] mu3 = rep_vector(0.0, N);
  vector[N] mu4 = rep_vector(0.0, N);
  mu2 += Intercept[1];
  mu3 += Intercept[2];
  mu4 += Intercept[3];
  for (n in 1 : N) {
    mu2[n] += r_1[J_1[n]] * Z_1[n];
  }
  for (n in 1 : N) {
    mu3[n] += r_2[J_2[n]] * Z_2[n];
  }
  for (n in 1 : N) {
    mu4[n] += r_3[J_3[n]] * Z_3[n];
  }
  for (n in 1 : N) {
    target += normal_lpdf(Y[n] | mu2[n] + mu3[n] + mu4[n], 1);
  }
  Intercept ~ student_t(3, 0, 2.5);
  sd ~ student_t(3, 0, 2.5);
  for (k in 1 : 3) {
    z[k] ~ std_normal();
  }
}
