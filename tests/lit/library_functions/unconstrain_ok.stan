// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
transformed data {
  vector[3] u = [0.4, -0.3, 1.1]';
  matrix[2, 3] um = [[0.4, -0.3, 1.1], [0.2, 0.5, -0.9]];
  vector[3] cu = [0.3, -0.2, 0.6]';
  vector[6] cz = [0.3, -0.2, 0.6, 0.1, 0.4, -0.5]';
  array[2] vector[3] ua = {u, 2 * u};
  if (max(abs(ordered_unconstrain(ordered_constrain(u)) - u)) > 1e-12)
    reject("ordered");
  if (max(abs(positive_ordered_unconstrain(positive_ordered_constrain(u)) - u))
      > 1e-12)
    reject("positive_ordered");
  if (max(abs(simplex_unconstrain(simplex_constrain(u)) - u)) > 1e-12)
    reject("simplex");
  if (max(abs(sum_to_zero_unconstrain(sum_to_zero_constrain(u)) - u)) > 1e-12)
    reject("sum_to_zero");
  if (max(abs(to_vector(sum_to_zero_unconstrain(sum_to_zero_constrain(um)) - um)))
      > 1e-12)
    reject("sum_to_zero matrix");
  if (max(abs(to_vector(stochastic_column_unconstrain(stochastic_column_constrain(um))
                        - um))) > 1e-12)
    reject("stochastic_column");
  if (max(abs(to_vector(stochastic_row_unconstrain(stochastic_row_constrain(um))
                        - um))) > 1e-12)
    reject("stochastic_row");
  if (max(abs(cholesky_factor_corr_unconstrain(cholesky_factor_corr_constrain(cu, 3))
              - cu)) > 1e-12)
    reject("cholesky_factor_corr");
  if (max(abs(corr_matrix_unconstrain(corr_matrix_constrain(cu, 3)) - cu)) > 1e-10)
    reject("corr_matrix");
  if (max(abs(cov_matrix_unconstrain(cov_matrix_constrain(cz, 3)) - cz)) > 1e-10)
    reject("cov_matrix");
  if (max(abs(cholesky_factor_cov_unconstrain(cholesky_factor_cov_constrain(cz, 3, 3))
              - cz)) > 1e-12)
    reject("cholesky_factor_cov");
  vector[3] unit = u / norm2(u);
  if (max(abs(unit_vector_unconstrain(unit) - unit)) > 1e-12)
    reject("unit_vector");
  array[2] vector[4] sa = {simplex_constrain(ua[1]), simplex_constrain(ua[2])};
  array[2] vector[3] back = simplex_unconstrain(sa);
  if (max(abs(back[1] - ua[1])) > 1e-12 || max(abs(back[2] - ua[2])) > 1e-12)
    reject("simplex array");
}
parameters {
  vector[3] p;
  matrix[2, 3] pm;
  vector[3] q;
  vector[6] z;
}
model {
  if (max(abs(ordered_unconstrain(ordered_constrain(p)) - p)) > 1e-9)
    reject("ordered param");
  if (max(abs(positive_ordered_unconstrain(positive_ordered_constrain(p)) - p))
      > 1e-9)
    reject("positive_ordered param");
  if (max(abs(simplex_unconstrain(simplex_constrain(p)) - p)) > 1e-9)
    reject("simplex param");
  if (max(abs(sum_to_zero_unconstrain(sum_to_zero_constrain(p)) - p)) > 1e-9)
    reject("sum_to_zero param");
  if (max(abs(to_vector(sum_to_zero_unconstrain(sum_to_zero_constrain(pm)) - pm)))
      > 1e-9)
    reject("sum_to_zero matrix param");
  if (max(abs(to_vector(stochastic_column_unconstrain(stochastic_column_constrain(pm))
                        - pm))) > 1e-9)
    reject("stochastic_column param");
  if (max(abs(to_vector(stochastic_row_unconstrain(stochastic_row_constrain(pm))
                        - pm))) > 1e-9)
    reject("stochastic_row param");
  if (max(abs(cholesky_factor_corr_unconstrain(cholesky_factor_corr_constrain(q, 3))
              - q)) > 1e-9)
    reject("cholesky_factor_corr param");
  if (max(abs(corr_matrix_unconstrain(corr_matrix_constrain(q, 3)) - q)) > 1e-7)
    reject("corr_matrix param");
  if (max(abs(cov_matrix_unconstrain(cov_matrix_constrain(z, 3)) - z)) > 1e-7)
    reject("cov_matrix param");
  if (max(abs(cholesky_factor_cov_unconstrain(cholesky_factor_cov_constrain(z, 3, 3))
              - z)) > 1e-9)
    reject("cholesky_factor_cov param");
  target += -0.5 * dot_self(p) - 0.5 * dot_self(to_vector(pm))
            - 0.5 * dot_self(q) - 0.5 * dot_self(z);
}
