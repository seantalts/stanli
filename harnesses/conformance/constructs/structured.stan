parameters {
  ordered[2] ordered_value;
  simplex[3] simplex_value;
  cholesky_factor_corr[2] correlation_cholesky;
}
model {
  target += ordered_value[1] + 0.5 * ordered_value[2];
  target += dot_product(simplex_value, [0.25, 0.5, 0.75]');
  target += sum(correlation_cholesky);
}
