functions {
  real f_lpdf(real y, real mu, real sigma) {
    if (mu <= 0 || sigma <= 0) return negative_infinity();
    real t = y - mu;
    if (t <= 0) return log(0.1);
    return normal_lpdf(t | 0, sigma);
  }
}
data { int N; vector[N] y; }
parameters { real<lower=0> mu; real<lower=0> sigma; }
model { for (n in 1:N) target += f_lpdf(y[n] | mu, sigma); }
