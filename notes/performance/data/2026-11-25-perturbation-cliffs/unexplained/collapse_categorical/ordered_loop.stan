data { int<lower=1> N; array[N] int<lower=1, upper=4> R; }
parameters { ordered[3] c; }
model {
  c ~ normal(0, 1.5);
  for (i in 1:N) R[i] ~ ordered_logistic(0, c);
}
