data {
  int N;
  vector[N] y;
  vector[N] x;
  array[N] int dec;
}
parameters {
  real alpha;
  real beta;
  real delta;
}
model {
  for (n in 1:N) {
    if (dec[n] == 1)
      target += wiener_lpdf(y[n] | alpha, 0.1, beta, delta * x[n]);
    else
      target += wiener_lpdf(y[n] | alpha, 0.1, 1 - beta, -delta * x[n]);
  }
}
