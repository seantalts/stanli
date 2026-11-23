functions {
  matrix assembled_covariance(vector x, real amplitude, real rate, int invalid) {
    int N = rows(x);
    matrix[N, N] K;
    for (i in 1:N) {
      K[i, i] = amplitude + 0.1;
      if (i < N) {
        for (j in (i + 1):N) {
          K[i, j] = amplitude * exp(-rate * square(x[i] - x[j]));
          K[j, i] = K[i, j];
        }
      }
    }
    if (invalid == 1) K[1, 1] = -1;
    if (invalid == 2 && N > 1) K[1, 2] += 0.25;
    return K;
  }
}
data {
  int<lower=1> N;
  vector[N] x;
  vector[N] y;
  int<lower=0, upper=2> invalid;
}
parameters {
  real<lower=0> amplitude;
  real<lower=0> rate;
  vector[N] z;
}
transformed parameters {
  matrix[N, N] K = assembled_covariance(x, amplitude, rate, invalid);
  matrix[N, N] L = cholesky_decompose(K);
  vector[N] mu = L * z;
}
model {
  amplitude ~ exponential(1);
  rate ~ exponential(1);
  z ~ std_normal();
  y ~ normal(mu, 1);
}
