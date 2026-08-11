data {
  int<lower=0, upper=2> B;
}
parameters {
  array[B] simplex[3] simp;
  array[B] corr_matrix[2] corr;
  array[B] cov_matrix[2] cov;
  array[B] cholesky_factor_corr[2] chol_corr;
  array[B] cholesky_factor_cov[2] chol_sq;
  array[B] cholesky_factor_cov[3, 2] chol_rect;
  array[B] sum_to_zero_vector[3] stz_vec;
  array[B] sum_to_zero_matrix[2, 3] stz_mat;
  array[B] sum_to_zero_matrix[1, 3] stz_row_zero;
  array[B] sum_to_zero_matrix[3, 1] stz_col_zero;
  real anchor;
}
model {
  for (b in 1:B) {
    target += b * (
      sum(simp[b]) + 2 * simp[b, 1]
      + sum(corr[b]) + 3 * corr[b, 1, 2]
      + sum(cov[b]) + 5 * cov[b, 1, 2]
      + sum(chol_corr[b]) + 7 * chol_corr[b, 2, 1]
      + sum(chol_sq[b]) + 11 * chol_sq[b, 2, 1]
      + sum(chol_rect[b]) + 13 * chol_rect[b, 3, 2]
      + sum(stz_vec[b]) + 17 * stz_vec[b, 2]
      + sum(stz_mat[b]) + 19 * stz_mat[b, 1, 1]
    );
  }
  target += anchor;
}
generated quantities {
  real proof = anchor;
}
