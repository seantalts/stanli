// A model-block local and a generated quantity share a name, so stanc gives
// both sizes the same name, v_1dim__: a log_prob local and a prepared
// transformed-data value.
data {
  int N;
  int r;
}
parameters {
  real q;
}
model {
  vector[r * N + 1] v = rep_vector(q, r * N + 1);
  target += normal_lpdf(v | 0, 1);
}
generated quantities {
  vector[r * N] v = rep_vector(2 * q, r * N);
}
