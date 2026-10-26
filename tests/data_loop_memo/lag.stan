data {
  int N;
  int K;
  int T;
  array[T] int flag;
  vector[T] x;
  vector[T] z;
  vector[N] sdv;
}
parameters {
  real mu;
}
model {
  mu ~ normal(0, 1);
  for (n in 1:N) {
    real s = sdv[n];
    real prev = 0;
    for (k in 1:K) {
      if (mu > 100) target += -1;
      for (t in 1:T) {
        if (flag[t] == 1) target += normal_lpdf(z[t] | mu * prev, s);
        prev = x[t];
      }
    }
  }
}
