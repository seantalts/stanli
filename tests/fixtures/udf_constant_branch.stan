functions {
  real f(real y, real mu, real s) {
    real r;
    if (y - mu < 0) {
      if (s == 0) r = -square(y - mu);
      else r = lgamma(y + exp(mu));
    } else {
      r = -abs(y - mu);
    }
    return r;
  }
}
data {
  int N;
  vector[N] y;
}
parameters {
  real mu;
}
transformed parameters {
  real s = 0;
}
model {
  for (n in 1:N) target += f(y[n], mu, s);
}
