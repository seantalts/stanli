// Each `target +=` adds its term once, including when it adds the same
// local variable again.
data {
  int N;
  vector[N] y;
}
parameters {
  real mu;
  real tau;
}
model {
  for (n in 1:N) {
    real lp = normal_lpdf(y[n] | mu, 1);
    target += lp;
    target += lp;
  }
  for (n in 1:N) {
    real lp = normal_lpdf(y[n] | tau, 1);
    target += lp;
    target += lp;
    target += lp;
  }
  real whole = normal_lpdf(y | tau, 2);
  target += whole;
  target += whole;
}
