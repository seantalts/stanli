// Structured declaration checks are distinct from parameter transforms.
// Data checks run during construction; transformed-parameter and generated-
// quantity checks run at their declaration sites for each draw.
data {
  int<lower=0> B;
  array[B] simplex[3] d_simplex;
  array[B] ordered[3] d_ordered;
  array[B] positive_ordered[3] d_positive_ordered;
  array[B] unit_vector[3] d_unit_vector;
  array[B] sum_to_zero_vector[3] d_sum_to_zero_vector;
  array[B] corr_matrix[2] d_corr;
  array[B] cov_matrix[2] d_cov;
  array[B] cholesky_factor_corr[2] d_cholesky_corr;
  array[B] cholesky_factor_cov[3, 2] d_cholesky_cov;
  array[B] sum_to_zero_matrix[2, 2] d_sum_to_zero_matrix;
}
parameters {
  real tp_shift;
  real gq_shift;
}
transformed parameters {
  simplex[3] tp_simplex = rep_vector((1.0 + tp_shift) / 3.0, 3);
  real<lower=0> tp_later = tp_shift;
}
model {
  target += tp_later + gq_shift;
}
generated quantities {
  sum_to_zero_vector[3] gq_sum_to_zero = rep_vector(gq_shift, 3);
}
