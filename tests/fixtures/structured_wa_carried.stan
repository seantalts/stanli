data {
  int<lower=1> N;
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
  array[T] vector[3] a;
  for (t in 1:T) a[t] = softmax([mu * t, 1, mu + t]');
  vector[N] c;
  c[1] = mu * x[1];
  for (n in 2:N) c[n] = c[n - 1] * 0.9 + mu * x[n];
}
