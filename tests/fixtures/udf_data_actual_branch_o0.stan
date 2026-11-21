functions {
  real hurdle_term(real y, real mu) {
    real lp;
    if (y == 0)
      lp = log(0.3);
    else
      lp = log(0.7) + normal_lpdf(y | mu, 1);
    return lp;
  }
}
data {
  int N;
  vector[N] y;
}
parameters {
  real mu;
}
model {
  for (n in 1:N) target += hurdle_term(y[n], mu);
}
