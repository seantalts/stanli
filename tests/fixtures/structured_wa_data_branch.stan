data {
  int<lower=2> N;
  vector[N] x;
  int<lower=1> T;
}
parameters {
  real mu;
}
model {
  mu ~ normal(0, 1);
}
generated quantities {
  array[T] vector[3] pad;
  for (t in 1:T) pad[t] = softmax([mu * t, 1, mu + t]');

  real tot = 0;
  vector[N] cum;
  for (n in 1:N) {
    if (x[n] > 0) {
      tot += mu * x[n];
    } else {
      tot -= 0.5 * mu;
    }
    cum[n] = tot;
  }

  matrix[N, N] tri = rep_matrix(0, N, N);
  for (j in 1:N) {
    real run = 0;
    for (i in 1:N) {
      if (i <= j) {
        run += mu * x[i];
        tri[i, j] = run;
      }
    }
  }

  real flips = 0;
  vector[N] path;
  for (n in 1:N) {
    if (mu * x[n] > flips) {
      flips += 1;
    }
    path[n] = flips;
  }
}
