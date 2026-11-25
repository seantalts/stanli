data {
  int N;
  vector[N] y;
}
parameters {
  real mu;
}
model {
  for (n in 1:N) {
    real lp;
    if (y[n] == 0)
      lp = log(0.3);
    else
      lp = log(0.7) + normal_lpdf(y[n] | mu, 1);
    target += lp;
  }
}
