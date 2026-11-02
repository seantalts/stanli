transformed data {
  matrix[2, 2] dA = [[2, 0.5], [0.5, 3]];
  matrix[2, 2] dB = [[1, 2], [0.5, -1]];
  matrix[3, 2] dT = [[1, 2], [3, 4], [5, 7.5]];
  vector[2] dv = [1, 2]';
  vector[3] dw = [0.2, -0.4, 0.7]';
  array[2] vector[3] dlam = {[-1.5, -0.25, -0.75]', [0.5, -2, -1]'};
  vector[3] dtheta = [0.2, 0.5, 0.3]';
  array[4] int csr_v = {1, 3, 2, 3};
  array[4] int csr_u = {1, 3, 4, 5};
  vector[4] csr_w = [1, 2, 3, 4]';
  vector[3] csr_b = [1, -1, 2]';
  matrix[2, 2] hmm_gamma = [[0.9, 0.1], [0.2, 0.8]];
  vector[2] hmm_rho = [0.6, 0.4]';
  matrix[2, 3] hmm_omega = [[-0.7, -1.2, -0.3], [-1.1, -0.4, -0.9]];
  matrix[2, 3] dlm_y = [[0.3, -0.2, 0.5], [0.1, 0.4, -0.6]];
  real data_score = determinant(dA) + log_determinant_spd(dA)
                    + sum(chol2inv(cholesky_decompose(dA)))
                    + sum(generalized_inverse(dT)) + sum(qr_Q(dT))
                    + sum(qr_R(dT)) + sum(qr_thin_Q(dT)) + sum(qr_thin_R(dT))
                    + sum(matrix_power(dA, 3)) + trace_quad_form(dA, dB)
                    + trace_quad_form(dA, dv) + trace_gen_quad_form(dB, dA, dB)
                    + trace_dot(dA, dB) + sum(matrix_exp_multiply(dA, dB))
                    + sum(scale_matrix_exp_multiply(0.3, dA, dB)) + norm1(dw)
                    + norm2(dw) + distance(dw, dtheta) + inc_beta(2.5, 1.5, 0.4)
                    + inv_inc_beta(2.5, 1.5, 0.4)
                    + hypergeometric_2F1(0.7, 1.5, 3.5, 0.4)
                    + hypergeometric_3F2({0.7, 1.0, 1.5}, {2.5, 3.0}, 0.4)
                    + hypergeometric_pFq({0.7, 1.0}, {2.5}, 0.4)
                    + int_step(2) + int_step(-1) + log_mix(dtheta, dw)
                    + log_mix(dtheta, dlam) + log_mix(0.3, -1.5, -0.25)
                    + discrete_range_cdf(3 | 1, 5) + discrete_range_lcdf(3 | 1, 5)
                    + discrete_range_lccdf(3 | 1, 5)
                    + quantile({1.0, 2.0, 3.0, 4.0, 5.0}, 0.3)
                    + sum(csr_to_dense_matrix(3, 3, csr_w, csr_v, csr_u))
                    + sum(csr_matrix_times_vector(3, 3, csr_w, csr_v, csr_u, csr_b))
                    + hmm_marginal(hmm_omega, hmm_gamma, hmm_rho)
                    + sum(hmm_hidden_state_prob(hmm_omega, hmm_gamma, hmm_rho))
                    + sum(quantile({1.0, 2.0, 3.0, 4.0, 5.0}, {0.2, 0.9}))
                    + gaussian_dlm_obs_lpdf(dlm_y | dB, dA, dv, dA, dv, dA);
}
parameters {
  vector[2] p;
  matrix[2, 2] pm;
  matrix[3, 2] pt;
  real<lower=0.5, upper=0.9> a;
  real<lower=0.1, upper=0.9> x;
  simplex[3] s;
  ordered[3] o;
  positive_ordered[3] po;
  sum_to_zero_vector[3] z;
  sum_to_zero_matrix[2, 3] zm;
  matrix[2, 2] sc_raw;
  matrix[2, 2] sr_raw;
  cholesky_factor_corr[3] L;
  corr_matrix[3] R;
  cov_matrix[3] Sg;
  cholesky_factor_cov[3] Lc;
  vector[3] lam;
  array[2] vector[3] lams;
  simplex[2] g1;
  simplex[2] g2;
  simplex[2] rho;
  matrix[2, 3] omega;
  vector<lower=0.3>[2] dlm_v;
  vector[2] dlm_m0;
}
transformed parameters {
  vector[2] s_free = simplex_unconstrain(s);
  vector[3] o_free = ordered_unconstrain(o);
  vector[3] po_free = positive_ordered_unconstrain(po);
  vector[2] z_free = sum_to_zero_unconstrain(z);
  matrix[1, 2] zm_free = sum_to_zero_unconstrain(zm);
  matrix[3, 2] sc = stochastic_column_constrain(sc_raw);
  matrix[2, 3] sr = stochastic_row_constrain(sr_raw);
  matrix[2, 2] sc_free = stochastic_column_unconstrain(sc);
  matrix[2, 2] sr_free = stochastic_row_unconstrain(sr);
  vector[3] L_free = cholesky_factor_corr_unconstrain(L);
  vector[3] R_free = corr_matrix_unconstrain(R);
  vector[6] Sg_free = cov_matrix_unconstrain(Sg);
  vector[6] Lc_free = cholesky_factor_cov_unconstrain(Lc);
  vector[3] u = (lam + 1.5) / norm2(lam + 1.5);
  vector[3] u_free = unit_vector_unconstrain(u);
  matrix[2, 2] S = crossprod(pm) + diag_matrix(rep_vector(1.0, 2));
  matrix[2, 2] hmm_gamma_p = [[g1[1], g1[2]], [g2[1], g2[2]]];
  matrix[2, 3] gen_inv = generalized_inverse(pt);
  matrix[2, 2] chol_inv = chol2inv(cholesky_decompose(S));
  matrix[2, 2] power = matrix_power(S, 2);
  matrix[3, 3] qr_q = qr_Q(pt);
  matrix[3, 2] qr_r = qr_R(pt);
  matrix[3, 2] thin_q = qr_thin_Q(pt);
  matrix[2, 2] thin_r = qr_thin_R(pt);
  matrix[2, 2] exp_mult = matrix_exp_multiply(S, pm);
  matrix[2, 2] scale_exp_mult = scale_matrix_exp_multiply(x, S, pm);
  matrix[3, 3] csr_dense = csr_to_dense_matrix(3, 3, to_vector(s) .* csr_w[1:3], {1, 3, 2}, {1, 2, 3, 4});
  vector[3] csr_prod = csr_matrix_times_vector(3, 3, append_row(s, x), csr_v, csr_u, lam);
  vector[5] matrix_results;
  vector[19] scalar_results;
  matrix_results[1] = determinant(S);
  matrix_results[2] = log_determinant_spd(S);
  matrix_results[3] = trace_quad_form(S, pm);
  matrix_results[4] = trace_quad_form(S, p);
  matrix_results[5] = trace_gen_quad_form(pm, S, pm);
  scalar_results[1] = trace_dot(S, pm);
  scalar_results[2] = norm1(p);
  scalar_results[3] = norm2(lam + 2);
  scalar_results[4] = distance(lam, s);
  scalar_results[5] = inc_beta(a, 2.5, x);
  scalar_results[6] = inv_inc_beta(a, 2.5, x);
  scalar_results[7] = hypergeometric_2F1(a, 1.5, 3.5, x);
  scalar_results[8] = hypergeometric_3F2({a, 1.0, 1.5}, {2.5, 3.0}, x);
  scalar_results[9] = hypergeometric_pFq({a, 1.0}, {2.5}, x);
  scalar_results[10] = int_step(lam[1]) + int_step(p[2] - 1);
  scalar_results[11] = log_mix(s, lam);
  scalar_results[12] = log_mix(s, lams);
  scalar_results[13] = log_mix(x, lam[1], lam[2]);
  scalar_results[14] = hmm_marginal(omega, hmm_gamma_p, rho);
  scalar_results[15] = trace_gen_quad_form(S, S, S);
  scalar_results[16] = gaussian_dlm_obs_lpdf(dlm_y | pm, S, dlm_v, S, dlm_m0, S);
  scalar_results[17] = gaussian_dlm_obs_lpdf(dlm_y | pm, S, diag_matrix(dlm_v), S, dlm_m0, S);
  scalar_results[18] = discrete_range_lcdf(3 | 1, 5);
  scalar_results[19] = quantile(dw, 0.4);
}
model {
  target += -0.5 * dot_self(p) - 0.5 * dot_self(to_vector(pm))
            - 0.5 * dot_self(to_vector(pt)) - 0.5 * dot_self(lam)
            - 0.5 * dot_self(to_vector(omega)) - 0.5 * dot_self(lams[1])
            - 0.5 * dot_self(lams[2]) - 0.5 * dot_self(dlm_m0);
  target += dot_product(s_free, [1, 2]') + dot_product(o_free, [3, 2, 1]')
            + dot_product(po_free, [1, 1, 2]') + dot_product(z_free, [2, 1]')
            + sum(zm_free) + sum(sc_free) + 2 * sum(sr_free)
            + dot_product(L_free, [1, 2, 3]') + dot_product(R_free, [3, 2, 1]')
            + dot_product(Sg_free, [1, 2, 3, 4, 5, 6]')
            + dot_product(Lc_free, [6, 5, 4, 3, 2, 1]')
            + dot_product(u_free, [1, 3, 2]');
  target += dot_product(matrix_results, linspaced_vector(5, 1, 5) / 5);
  target += dot_product(scalar_results, linspaced_vector(19, 1, 19) / 19);
  target += dot_product(to_vector(gen_inv), linspaced_vector(6, 1, 6) / 6)
            + dot_product(to_vector(chol_inv), linspaced_vector(4, 1, 4) / 4)
            + dot_product(to_vector(power), reverse(linspaced_vector(4, 1, 4)) / 4)
            + dot_product(to_vector(qr_q), linspaced_vector(9, 1, 9) / 9)
            + dot_product(to_vector(qr_r), linspaced_vector(6, 1, 6) / 6)
            + dot_product(to_vector(thin_q), reverse(linspaced_vector(6, 1, 6)) / 6)
            + dot_product(to_vector(thin_r), linspaced_vector(4, 1, 4) / 4)
            + dot_product(to_vector(exp_mult), linspaced_vector(4, 1, 4) / 4)
            + dot_product(to_vector(scale_exp_mult), reverse(linspaced_vector(4, 1, 4)) / 4)
            + dot_product(to_vector(csr_dense), linspaced_vector(9, 1, 9) / 9)
            + dot_product(csr_prod, [1, 2, 3]' / 3);
  dlm_y ~ gaussian_dlm_obs(pm, S, dlm_v, S, dlm_m0, S);
  target += gaussian_dlm_obs_lupdf(dlm_y | pm, S, diag_matrix(dlm_v), S, dlm_m0, S);
}
generated quantities {
  real gq_quantile = quantile(lam, 0.35);
  array[2] real gq_quantiles = quantile(lam, {0.1, 0.8});
  matrix[2, 3] gq_hidden = hmm_hidden_state_prob(omega, hmm_gamma_p, rho);
  int gq_yule = yule_simon_rng(a + 1);
}
