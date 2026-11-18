// Truncated user distributions with int bounds, in each shape that stanc3
// writes for them.
functions {
  real foo_lpdf(real y, real mu) {
    return -square(y - mu);
  }
  real foo_lpdf(vector y, real mu) {
    return -dot_self(y - mu);
  }
  real foo_lpdf(real y, vector mu) {
    return -dot_self(y - mu);
  }
  real foo_lcdf(real y, real mu) {
    return -1.7 / y - 0.1 * mu;
  }
  real foo_lccdf(real y, real mu) {
    return -0.02 * y - 0.1 * mu;
  }
  real foo_lcdf(real y, vector mu) {
    return -1.7 / y - 0.1 * sum(mu);
  }
  real foo_lccdf(real y, vector mu) {
    return -0.02 * y - 0.1 * sum(mu);
  }
  real bar_lpmf(int n, real lam) {
    return n * log(lam) - lam;
  }
  real bar_lcdf(int n, real lam) {
    return -1.5 / (n + 1) - 0.1 * lam;
  }
  real bar_lccdf(int n, real lam) {
    return -0.25 * n - 0.1 * lam;
  }
}
data {
  int L;
  int k;
}
parameters {
  real<lower=1, upper=5> y;
  real mu;
  vector<lower=1, upper=5>[2] z;
  vector[2] nu;
  real<lower=0> lam;
}
model {
  y ~ foo(mu) T[1, ];
  y ~ foo(mu) T[, 5];
  y ~ foo(mu) T[1, 5];
  y ~ foo(mu) T[L, ];
  z ~ foo(mu) T[1, 5];
  y ~ foo(nu) T[1, 5];
  k ~ bar(lam) T[1, 8];
}
