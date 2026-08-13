data {
  int<lower=1> N;
  vector[N] x;
}
parameters {
  real theta;
}
model {
  target += normal_lpdf(theta | sum(x), 1);
}
