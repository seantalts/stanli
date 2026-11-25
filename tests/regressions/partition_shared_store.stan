data {
  int N;
  vector[N] y;
  matrix[N, 1] X;
}
parameters {
  vector[1] b;
  real a;
}
transformed parameters {
  vector[N] mu;
  vector[N] sg = rep_vector(1.0, N);
  for (n in 1:N) {
    mu[n] = X[n] * b;
    sg[n] = exp(a);
  }
}
model {
  a ~ normal(0, 1);
  b ~ normal(0, 1);
  y ~ normal(mu, sg);
}
