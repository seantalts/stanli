data {
  int<lower=1> N;
  int<lower=1> T;
}
parameters {
  real mu;
}
model {
  mu ~ normal(0, 1);
}
generated quantities {
  array[N, T] real a;
  for (i in 1:N) {
    for (t in 1:T) {
      a[i, t] = 0;
    }
  }
}
