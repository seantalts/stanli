"""Domain-valid integrated probes for functions outside the scalar generator.

Each named result is retained in the draw output, and container projections
use unequal weights so permutations cannot hide behind a plain sum.
"""

HELPERS = """
  real logical_probe(real x) {
    if (x > 0) return logical_negation(x > 0);
    return 2.0;
  }
  real probe(real x) { return x; }
  real probe(vector x) {
    real out = 0;
    for (i in 1:num_elements(x)) out += (1 + i * 0.125) * x[i];
    return out;
  }
  real probe(row_vector x) {
    real out = 0;
    for (i in 1:num_elements(x)) out += (1 + i * 0.125) * x[i];
    return out;
  }
  real probe(matrix x) {
    real out = 0;
    for (j in 1:cols(x)) for (i in 1:rows(x))
      out += (1 + (i + (j - 1) * rows(x)) * 0.125) * x[i,j];
    return out;
  }
  real probe(array[] real x) {
    real out = 0;
    for (i in 1:size(x)) out += (1 + i * 0.125) * x[i];
    return out;
  }
  real probe(array[] int x) {
    real out = 0;
    for (i in 1:size(x)) out += (1 + i * 0.125) * x[i];
    return out;
  }
  real probe(array[] vector x) {
    real out = 0;
    for (i in 1:size(x)) out += (1 + i * 0.125) * probe(x[i]);
    return out;
  }
"""

MATRIX_PREAMBLE = """
  vector[2] x = [1 + 0.0625 * theta[1], 2 + 0.0625 * theta[2]]';
  vector[3] v = [0.5 + 0.0625 * theta[3], 0.7 + 0.0625 * theta[4], 1.0 + 0.0625 * theta[5]]';
  matrix[2,3] m = [[x[1], v[1], v[2]], [x[2], v[3], 0.25 + 0.0625 * theta[6]]];
  matrix[2,2] s = [[2 + square(x[1]), 0.25], [0.25, 3 + square(x[2])]];
  matrix[2,2] l = cholesky_decompose(s);
  real rho = 0.2 + 0.01 * theta[7];
  matrix[2,2] corr = [[1, rho], [rho, 1]];
  matrix[2,2] lcorr = cholesky_decompose(corr);
  vector[2] p = softmax(x);
"""

PREAMBLE = {
    "containers": MATRIX_PREAMBLE,
    "multivariate": MATRIX_PREAMBLE,
    "data_functions": """
  array[2,3] real a = {{1,2,3},{4,5,6}};
  matrix[2,3] m = [[1,0,2],[0,3,4]];
  vector[3] v = [0.5,1.5,2.5]';
""",
    "rng_functions": MATRIX_PREAMBLE,
}

GROUPS = {
    "containers": {
        "add_diag": "add_diag(s, x)",
        "append_array": "append_array({x[1], x[2]}, {v[1]})",
        "append_col": "append_col(m, x)",
        "append_row": "append_row(m, v')",
        "block": "block(m, 1, 2, 2, 2)",
        "cholesky_decompose": "cholesky_decompose(s)",
        "col": "col(m, 2)", "cols": "cols(m)",
        "columns_dot_product": "columns_dot_product(m, 2 * m)",
        "columns_dot_self": "columns_dot_self(m)",
        "crossprod": "crossprod(m)",
        "csr_matrix_times_vector": "csr_matrix_times_vector(2, 3, [1,2,3,4]', {1,3,2,3}, {1,3,5}, v)",
        "cumulative_sum": "cumulative_sum(v)",
        "diag_matrix": "diag_matrix(x)",
        "diag_post_multiply": "diag_post_multiply(m, v)",
        "diag_pre_multiply": "diag_pre_multiply(x, m)",
        "diagonal": "diagonal(m)", "dims": "dims(m)",
        "dot_product": "dot_product(x, 2 * x)", "dot_self": "dot_self(x)",
        "eigenvalues_sym": "eigenvalues_sym(s)",
        "eigenvectors_sym": "eigenvectors_sym(s)",
        "gp_exp_quad_cov": "gp_exp_quad_cov({0.0, 1.0}, x[1], x[2])",
        "gp_exponential_cov": "gp_exponential_cov({0.0, 1.0}, x[1], x[2])",
        "gp_matern32_cov": "gp_matern32_cov({0.0, 1.0}, x[1], x[2])",
        "gp_matern52_cov": "gp_matern52_cov({0.0, 1.0}, x[1], x[2])",
        "logical_negation": "logical_probe(theta[1])",
        "head": "head(v, 2)", "inverse": "inverse(s)",
        "inverse_spd": "inverse_spd(s)",
        "log_determinant": "log_determinant(s)",
        "log_softmax": "log_softmax(v)",
        "matrix_exp": "matrix_exp(-s)",
        "max": "max(v)", "min": "min(v)", "mean": "mean(v)",
        "mdivide_left": "mdivide_left(s, x)",
        "mdivide_left_spd": "mdivide_left_spd(s, x)",
        "mdivide_left_tri_low": "mdivide_left_tri_low(l, x)",
        "mdivide_right": "mdivide_right(x', s)",
        "mdivide_right_spd": "mdivide_right_spd(x', s)",
        "mdivide_right_tri_low": "mdivide_right_tri_low(x', l)",
        # `s`, not the Cholesky factor `l`: the upper triangle stan-math drops
        # has to be non-zero for the probe to tell tril(A) A' from A A'.
        "multiply_lower_tri_self_transpose": "multiply_lower_tri_self_transpose(s)",
        "num_elements": "num_elements(m)", "prod": "prod(v)",
        "quad_form": "quad_form(s, x)",
        "quad_form_diag": "quad_form_diag(s, x)",
        "quad_form_sym": "quad_form_sym(s, s)",
        "rep_array": "rep_array(x, 3)",
        "rep_matrix": "rep_matrix(x, 3)",
        "rep_row_vector": "rep_row_vector(x[1], 3)",
        "rep_vector": "rep_vector(x[1], 3)",
        "reverse": "reverse(v)", "row": "row(m, 2)", "rows": "rows(m)",
        "rows_dot_product": "rows_dot_product(m, 2 * m)",
        "rows_dot_self": "rows_dot_self(m)",
        "sd": "sd(v)", "segment": "segment(v, 2, 2)",
        "size": "size({x[1], x[2]})", "softmax": "softmax(v)",
        "sub_col": "sub_col(m, 1, 2, 2)", "sum": "sum(m)",
        "tail": "tail(v, 2)", "tcrossprod": "tcrossprod(m)",
        "to_array_1d": "to_array_1d(v)",
        "to_matrix": "to_matrix(v, 1, 3)",
        "to_row_vector": "to_row_vector(m)", "to_vector": "to_vector(m)",
        "transpose": "transpose(m)", "variance": "variance(v)",
        "operator.^": "v .^ (1.5 + 0.0625 * theta[8])",
        "operator./": "m ./ (1 + m)",
        "operator.*": "m .* (1 + m)",
        "operator/": "x' / s", "operator\\": "s \\ x",
    },
    "data_functions": {
        "choose": "choose(8, 3)",
        "ceil": "ceil(v)", "floor": "floor(v)", "round": "round(v)",
        "trunc": "trunc(v)", "step": "step(-0.5)",
        "is_inf": "is_inf(positive_infinity())",
        "is_nan": "is_nan(0.0)",
        # The logical_* library spellings of the comparison operators, with
        # mixed int/real operands to exercise promotion.
        "logical_eq": "logical_eq(2.5, 2.5)",
        "logical_neq": "logical_neq(1, 2.5)",
        "logical_lt": "logical_lt(1, 2.5)",
        "logical_lte": "logical_lte(2.5, 2.5)",
        "logical_gt": "logical_gt(3.5, 2)",
        "logical_gte": "logical_gte(2, 3.5)",
        "logical_and": "logical_and(2, 0)",
        "logical_or": "logical_or(0, 3)",
        "not_a_number": "is_nan(not_a_number())",
        "machine_precision": "machine_precision()",
        "negative_infinity": "is_inf(negative_infinity())",
        "positive_infinity": "is_inf(positive_infinity())", "pi": "pi()",
        "linspaced_array": "linspaced_array(3, -1.5, 2.5)",
        "linspaced_int_array": "linspaced_int_array(5, 2, 3)",
        "linspaced_vector": "linspaced_vector(4, 0.0, 1.0)",
        "linspaced_row_vector": "linspaced_row_vector(3, 2.0, 8.0)",
        "zeros_vector": "zeros_vector(3)",
        "zeros_row_vector": "zeros_row_vector(3)",
        "zeros_int_array": "zeros_int_array(3)",
        "ones_array": "ones_array(3)", "ones_vector": "ones_vector(3)",
        "ones_row_vector": "ones_row_vector(3)",
        "identity_matrix": "identity_matrix(3)",
        "csr_extract_u": "csr_extract_u(m)",
        "csr_extract_v": "csr_extract_v(m)",
        "csr_extract_w": "csr_extract_w(m)",
        "to_matrix": "to_matrix(a)",
        "hypergeometric_lpmf": "hypergeometric_lpmf(2 | 4, 5, 7)",
        "discrete_range_lpmf": "discrete_range_lpmf(2 | 1, 4)",
    },
    "multivariate": {
        "bernoulli_logit_glm_lpmf": "bernoulli_logit_glm_lpmf(binary | design, 0.2, x)",
        "binomial_logit_glm_lpmf": "binomial_logit_glm_lpmf(count | trials, design, 0.2, x)",
        "categorical_logit_glm_lpmf": "categorical_logit_glm_lpmf(count | design, x, s)",
        "normal_id_glm_lpdf": "normal_id_glm_lpdf(x | design, 0.2, x, 1.3)",
        "neg_binomial_2_log_glm_lpmf": "neg_binomial_2_log_glm_lpmf(count | design, 0.2, x, 1.3)",
        "poisson_log_glm_lpmf": "poisson_log_glm_lpmf(count | design, 0.2, x)",
        "ordered_logistic_glm_lpmf": "ordered_logistic_glm_lpmf(count | design, x, [-0.5, 1.5]')",
        "categorical_lpmf": "categorical_lpmf(1 | p)",
        "categorical_logit_lpmf": "categorical_logit_lpmf(1 | x)",
        "dirichlet_lpdf": "dirichlet_lpdf(p | x)",
        "dirichlet_multinomial_lpmf": "dirichlet_multinomial_lpmf(count | x)",
        "lkj_corr_cholesky_lpdf": "lkj_corr_cholesky_lpdf(lcorr | 1.3)",
        "lkj_corr_lpdf": "lkj_corr_lpdf(corr | 1.3)",
        "lkj_cov_lpdf": "lkj_cov_lpdf(s | [0.0,0.1]', [1.0,1.2]', 1.3)",
        "multi_gp_cholesky_lpdf": "multi_gp_cholesky_lpdf(s | l, x)",
        "multi_gp_lpdf": "multi_gp_lpdf(s | s, x)",
        "multi_normal_cholesky_lpdf": "multi_normal_cholesky_lpdf(x | [0.0,0.1]', l)",
        "multi_normal_lpdf": "multi_normal_lpdf(x | [0.0,0.1]', s)",
        "multi_normal_prec_lpdf": "multi_normal_prec_lpdf(x | [0.0,0.1]', s)",
        "multi_student_t_cholesky_lpdf": "multi_student_t_cholesky_lpdf(x | 4.0, [0.0,0.1]', l)",
        "multi_student_t_lpdf": "multi_student_t_lpdf(x | 4.0, [0.0,0.1]', s)",
        "multinomial_logit_lpmf": "multinomial_logit_lpmf(count | x)",
        "multinomial_lpmf": "multinomial_lpmf(count | p)",
        "ordered_logistic_lpmf": "ordered_logistic_lpmf(2 | x[1], [-0.5,1.5]')",
        "ordered_probit_lpmf": "ordered_probit_lpmf(2 | x[1], [-0.5,1.5]')",
        "wishart_lpdf": "wishart_lpdf(s | 4.0, corr)",
        "wishart_cholesky_lpdf": "wishart_cholesky_lpdf(l | 4.0, lcorr)",
        "inv_wishart_lpdf": "inv_wishart_lpdf(s | 4.0, corr)",
        "inv_wishart_cholesky_lpdf": "inv_wishart_cholesky_lpdf(l | 4.0, lcorr)",
    },
    "rng_functions": {
        "normal_rng": "normal_rng(theta[1], 1.3)",
        "std_normal_rng": "std_normal_rng()",
        "lognormal_rng": "lognormal_rng(theta[1], 1.3)",
        "uniform_rng": "uniform_rng(-1.0, 2.0)",
        "gamma_rng": "gamma_rng(2.0, 1.3)",
        "inv_gamma_rng": "inv_gamma_rng(2.0, 1.3)",
        "beta_rng": "beta_rng(2.0, 3.0)",
        "exponential_rng": "exponential_rng(1.3)",
        "chi_square_rng": "chi_square_rng(3.0)",
        "cauchy_rng": "cauchy_rng(theta[1], 1.3)",
        "double_exponential_rng": "double_exponential_rng(theta[1], 1.3)",
        "logistic_rng": "logistic_rng(theta[1], 1.3)",
        "student_t_rng": "student_t_rng(4.0, theta[1], 1.3)",
        "weibull_rng": "weibull_rng(2.0, 1.3)",
        "bernoulli_rng": "bernoulli_rng(0.4)",
        "bernoulli_logit_rng": "bernoulli_logit_rng(theta[1])",
        "binomial_rng": "binomial_rng(3, 0.4)",
        "poisson_rng": "poisson_rng(2.0)",
        "poisson_log_rng": "poisson_log_rng(theta[1])",
        "neg_binomial_2_rng": "neg_binomial_2_rng(2.0, 1.3)",
        "neg_binomial_2_log_rng": "neg_binomial_2_log_rng(theta[1], 1.3)",
        "multi_normal_rng": "multi_normal_rng(x, s)",
        "multi_normal_cholesky_rng": "multi_normal_cholesky_rng(x, l)",
        "categorical_rng": "categorical_rng(p)",
        "categorical_logit_rng": "categorical_logit_rng(x)",
        "gumbel_rng": "gumbel_rng(theta[1], 1.3)",
        "dirichlet_rng": "dirichlet_rng(v)",
        "beta_binomial_rng": "beta_binomial_rng(5, 2.0, 3.0)",
    },
}
