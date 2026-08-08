// Ordinal regression. The cutpoint vector is shared by every
// observation rather than broadcast per-lane, which is the shape the
// recorder had to learn (VecMask in densities.cpp); stan-math also
// reaches its cutpoint partials through a vector-of-vectors edge and
// asks scalar_seq_view for a mutable pointer into the outcome, so this
// one model exercises all three.
data {
  int<lower=1> N;
  int<lower=2> K;
  array[N] int<lower=1, upper=K> y;
}
parameters {
  vector[N] lambda;
  ordered[K - 1] c;
}
model {
  lambda ~ normal(0, 2);
  y ~ ordered_logistic(lambda, c);
}
