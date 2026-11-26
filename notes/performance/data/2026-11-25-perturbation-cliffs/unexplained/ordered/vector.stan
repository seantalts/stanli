data { int<lower=1> N; array[N] int<lower=1, upper=4> R; }
parameters { ordered[3] c; }
model {
  c ~ normal(0, 1.5);
  R ~ ordered_logistic(rep_vector(0, N), c);
}
