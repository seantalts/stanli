functions {
  real partial_log_lik_lpmf(array[] int seq, int start, int end, data vector Y,
                              real Intercept, real sigma, data array[] int J_1,
                              data vector Z_1_1, data vector Z_1_2, data vector Z_1_3, data vector Z_1_4, data vector Z_1_5, data vector Z_1_6, data vector Z_1_7, data vector Z_1_8, data vector Z_1_9, data vector Z_1_10, data vector Z_1_11, data vector Z_1_12, data vector Z_1_13, data vector Z_1_14, data vector Z_1_15, data vector Z_1_16,
                              vector r_1_1, vector r_1_2, vector r_1_3, vector r_1_4, vector r_1_5, vector r_1_6, vector r_1_7, vector r_1_8, vector r_1_9, vector r_1_10, vector r_1_11, vector r_1_12, vector r_1_13, vector r_1_14, vector r_1_15, vector r_1_16) {
    int N = end - start + 1;
    vector[N] mu = rep_vector(0.0, N);
    mu += Intercept;
    for (n in 1 : N) {
      int nn = n + start - 1;
      mu[n] += r_1_1[J_1[nn]] * Z_1_1[nn]
               + r_1_2[J_1[nn]] * Z_1_2[nn]
               + r_1_3[J_1[nn]] * Z_1_3[nn]
               + r_1_4[J_1[nn]] * Z_1_4[nn]
               + r_1_5[J_1[nn]] * Z_1_5[nn]
               + r_1_6[J_1[nn]] * Z_1_6[nn]
               + r_1_7[J_1[nn]] * Z_1_7[nn]
               + r_1_8[J_1[nn]] * Z_1_8[nn]
               + r_1_9[J_1[nn]] * Z_1_9[nn]
               + r_1_10[J_1[nn]] * Z_1_10[nn]
               + r_1_11[J_1[nn]] * Z_1_11[nn]
               + r_1_12[J_1[nn]] * Z_1_12[nn]
               + r_1_13[J_1[nn]] * Z_1_13[nn]
               + r_1_14[J_1[nn]] * Z_1_14[nn]
               + r_1_15[J_1[nn]] * Z_1_15[nn]
               + r_1_16[J_1[nn]] * Z_1_16[nn];
    }
    return normal_lpdf(Y[start : end] | mu, sigma);
  }
}
data {
  int<lower=1> N;
  int<lower=1> N_1;
  vector[N] Y;
  array[N] int<lower=1, upper=N_1> J_1;
  vector[N] Z_1_1;
  vector[N] Z_1_2;
  vector[N] Z_1_3;
  vector[N] Z_1_4;
  vector[N] Z_1_5;
  vector[N] Z_1_6;
  vector[N] Z_1_7;
  vector[N] Z_1_8;
  vector[N] Z_1_9;
  vector[N] Z_1_10;
  vector[N] Z_1_11;
  vector[N] Z_1_12;
  vector[N] Z_1_13;
  vector[N] Z_1_14;
  vector[N] Z_1_15;
  vector[N] Z_1_16;
}
transformed data {
  array[N] int seq = linspaced_int_array(N, 1, N);
}
parameters {
  real Intercept;
  real<lower=0> sigma;
  vector[N_1] r_1_1;
  vector[N_1] r_1_2;
  vector[N_1] r_1_3;
  vector[N_1] r_1_4;
  vector[N_1] r_1_5;
  vector[N_1] r_1_6;
  vector[N_1] r_1_7;
  vector[N_1] r_1_8;
  vector[N_1] r_1_9;
  vector[N_1] r_1_10;
  vector[N_1] r_1_11;
  vector[N_1] r_1_12;
  vector[N_1] r_1_13;
  vector[N_1] r_1_14;
  vector[N_1] r_1_15;
  vector[N_1] r_1_16;
}
model {
  target += partial_log_lik_lpmf(seq | 1, N, Y, Intercept, sigma, J_1, Z_1_1, Z_1_2, Z_1_3, Z_1_4, Z_1_5, Z_1_6, Z_1_7, Z_1_8, Z_1_9, Z_1_10, Z_1_11, Z_1_12, Z_1_13, Z_1_14, Z_1_15, Z_1_16,
                                  r_1_1, r_1_2, r_1_3, r_1_4, r_1_5, r_1_6, r_1_7, r_1_8, r_1_9, r_1_10, r_1_11, r_1_12, r_1_13, r_1_14, r_1_15, r_1_16);
  Intercept ~ student_t(3, 0, 2.5);
  sigma ~ student_t(3, 0, 2.5);
  r_1_1 ~ std_normal();
  r_1_2 ~ std_normal();
  r_1_3 ~ std_normal();
  r_1_4 ~ std_normal();
  r_1_5 ~ std_normal();
  r_1_6 ~ std_normal();
  r_1_7 ~ std_normal();
  r_1_8 ~ std_normal();
  r_1_9 ~ std_normal();
  r_1_10 ~ std_normal();
  r_1_11 ~ std_normal();
  r_1_12 ~ std_normal();
  r_1_13 ~ std_normal();
  r_1_14 ~ std_normal();
  r_1_15 ~ std_normal();
  r_1_16 ~ std_normal();
}
