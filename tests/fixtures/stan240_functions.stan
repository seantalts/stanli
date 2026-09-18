functions {
  real new_functions(real q) {
    vector[3] p = inv_logit([q - 0.5, q + 0.1, q + 0.8]');
    row_vector[3] mu = [0.1, 0.2, 0.3];
    array[3] real sigma = {1.1, 1.2, 1.3};
    real result = sum(student_t_qf(p, 5.0, mu, sigma));
    result += poisson_binomial_lpmf(1 | p);
    result += poisson_binomial_lpmf({0, 2} | to_array_1d(p));
    result += poisson_binomial_cdf(1 | p');
    result += poisson_binomial_lcdf({1, 2} | p);
    result += poisson_binomial_lccdf(1 | p);
    return result;
  }
}
transformed data {
  real data_result = new_functions(0.15);
}
parameters { real q; }
transformed parameters {
  real result = new_functions(q);
  real branch_result;
  if (q > 0) branch_result = new_functions(q + 0.1);
  else branch_result = new_functions(q - 0.1);
}
model {
  target += result + branch_result + data_result;
}
generated quantities {
  real output_result = new_functions(q);
  int draw = poisson_binomial_rng(inv_logit([q - 0.5, q + 0.1, q + 0.8]'));
}
