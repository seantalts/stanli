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
  vector[N] run;
  vector[N] run2;
  real picked;
  real picked2;
  real vs;
  real ws;
  vector[N] run3;
  {
    int k = 0;
    real acc = 0;
    for (n in 1:N) {
      if (x[n] > 0) k += 1;
      acc = 0.5 * acc + mu * x[n];
      run[n] = acc;
    }
    vector[k] v = rep_vector(mu, k);
    vs = sum(v);
    picked = x[k];

    array[N] int cnt = rep_array(0, N);
    real acc2 = 0;
    run2[1] = 0;
    for (n in 2:N) {
      cnt[n] = cnt[n - 1] + (x[n] > 0);
      acc2 = acc2 * mu + x[n];
      run2[n] = acc2;
    }
    vector[cnt[N]] w = rep_vector(acc2, cnt[N]);
    ws = sum(w);
    picked2 = x[cnt[N] + 1];
  }
  real acc3 = 1;
  for (n in 1:N) {
    acc3 = acc3 * 0.9 + mu * x[n];
    run3[n] = acc3;
  }
}
