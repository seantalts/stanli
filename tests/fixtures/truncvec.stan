// Vectorized truncation, which the scalar trunc.stan fixture does not
// reach. stanc3 writes the two forms differently, and each one used a
// construct the reader rejected:
//
//   scalar location: the normalizer is FnLength(y) * log_diff_exp(...),
//     and FnLength is a compiler-internal, not a stan-library name.
//   container location with a literal scale: stanc3 loops over the
//     elements and hoists the scale into a temporary it declares
//     (Unsized UReal), which carries no size expression.
//
// Neither shape compiled before 0.4.1. Both are covered here.
data {
  int N;
  vector[N] y;
}
parameters {
  real mu;
  vector[N] theta;
  real<lower=0> sigma;
}
model {
  y ~ normal(mu, sigma) T[0, 10];
  y ~ normal(theta, 1) T[0, 10];
}
