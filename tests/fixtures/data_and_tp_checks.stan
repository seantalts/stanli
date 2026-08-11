// Bound checks happen in two distinct phases: data constraints reject during
// construction, while constrained transformed parameters reject the draw at
// log_prob/write_array evaluation time.
data {
  real<lower=0> d;
  real raw;
  int<lower=0> N;
  int<lower=0> M;
  vector[M] lo;
  int<lower=0> R;
  int<lower=0> C;
  int<lower=0> BR;
  int<lower=0> BC;
  matrix[BR, BC] matrix_lo;
}
parameters {
  real x;
}
transformed parameters {
  real<lower=0> z = x;
  real<lower=0> from_data = raw;
  vector<lower=lo>[N] bounded = rep_vector(x, N);
  matrix<lower=matrix_lo>[R, C] bounded_matrix = rep_matrix(x, R, C);
}
model {
  target += d + z + from_data + sum(bounded) + sum(bounded_matrix);
}
