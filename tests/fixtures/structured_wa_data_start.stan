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

  real acc = 0;
  vector[N] run;
  for (n in 1:N) {
    acc = 0.5 * acc + mu * x[n];
    run[n] = acc;
  }

  vector[N] w = x;
  for (n in 2:N) w[n] = w[n - 1] * mu + w[n];

  int k = 0;
  vector[N] s;
  for (n in 1:N) {
    k += n;
    s[n] = mu * k;
  }
  vector[N] f;
  f[1] = 1;
  for (n in 2:N) f[n] = f[n - 1] * mu + x[n];
  real tail = w[N] + acc + k + f[N];
}
