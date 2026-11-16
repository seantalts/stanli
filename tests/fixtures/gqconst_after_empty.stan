// A zero-size output ahead of a generated quantity the optimizer folds to a
// constant: the empty variable writes nothing, so the folded write belongs
// to w, not to z.
data {
  int N;
}
parameters {
  real mu;
}
model {
  mu ~ normal(0, 1);
}
generated quantities {
  array[N] vector[2] z;
  for (n in 1:N) z[n] = rep_vector(mu, 2);
  real w = 3.0;
}
