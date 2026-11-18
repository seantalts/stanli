functions {
  real partial_log_lik_lpmf(array[] int seq, int start, int end, data vector Y,
                            real Intercept, real sigma, data array[] int J_1,
                            data vector Z_1_1, data vector Z_1_2, vector r_1_1,
                            vector r_1_2) {
    int N = end - start + 1;
    vector[N] mu = rep_vector(0.0, N);
    mu += Intercept;
    for (n in 1 : N) {
      int nn = n + start - 1;
      mu[n] += r_1_1[J_1[nn]] * Z_1_1[nn] + r_1_2[J_1[nn]] * Z_1_2[nn];
    }
    vector[N] eta;
    for (n in 1 : N) {
      int nn = n + start - 1;
      eta[n] = mu[n] * exp(-Z_1_2[nn]);
    }
    return normal_lpdf(Y[start : end] | eta, sigma);
  }
}
data {
  int<lower=1> N;
  int<lower=1> N_1;
  vector[N] Y;
  array[N] int<lower=1, upper=N_1> J_1;
  vector[N] Z_1_1;
  vector[N] Z_1_2;
  int<lower=0, upper=1> direct;
}
transformed data {
  array[N] int seq = linspaced_int_array(N, 1, N);
}
parameters {
  real Intercept;
  real<lower=0> sigma;
  vector[N_1] r_1_1;
  vector[N_1] r_1_2;
}
model {
  if (direct) {
    target += partial_log_lik_lpmf(seq | 1, N, Y, Intercept, sigma, J_1, Z_1_1,
                                   Z_1_2, r_1_1, r_1_2);
  } else {
    target += reduce_sum(partial_log_lik_lpmf, seq, 1, Y, Intercept, sigma, J_1,
                         Z_1_1, Z_1_2, r_1_1, r_1_2);
  }
  r_1_1 ~ std_normal();
  r_1_2 ~ std_normal();
}
