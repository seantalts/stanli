data {
  int N;
  vector[N] y;
}
parameters {
  real alpha;
  real beta;
  real delta;
}
model {
  y ~ wiener(alpha, 0.1, beta, delta);
}
