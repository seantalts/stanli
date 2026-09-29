functions {
  real selected_rng(data int family, real a, real b) {
    real value = 0;
    if (family == 0) value = std_normal_rng();
    else if (family == 1) value = gamma_rng(a, b);
    else if (family == 2) value = inv_gamma_rng(a, b);
    else if (family == 3) value = beta_rng(a, b);
    else if (family == 4) value = chi_square_rng(a);
    else if (family == 5) value = cauchy_rng(a, b);
    else if (family == 6) value = double_exponential_rng(a, b);
    else if (family == 7) value = logistic_rng(a, b);
    else if (family == 8) value = weibull_rng(a, b);
    else if (family == 9) value = neg_binomial_2_rng(a, b);
    else if (family == 10) value = neg_binomial_2_log_rng(a, b);
    return value;
  }
}
data { int<lower=0, upper=10> family; int<lower=0, upper=1> region; }
parameters { real a; real b; }
model { a ~ normal(0, 1); b ~ normal(0, 1); }
generated quantities {
  real before = normal_rng(0, 1);
  real draw = 0;
  if (region) {
    int i = 0;
    while (i < 1) {
      draw = selected_rng(family, a, b);
      i += 1;
    }
  } else {
    draw = selected_rng(family, a, b);
  }
  real after = normal_rng(0, 1);
}
