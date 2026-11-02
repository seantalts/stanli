// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL offset_multiplier_constrain: multiplier[2, 1] is -1, but must be positive finite!
parameters {
  real y;
}
model {
  matrix[2, 2] sigma = rep_matrix(1 + y * 0, 2, 2);
  sigma[2, 1] = -1 + y * 0;
  matrix[2, 2] m = offset_multiplier_unconstrain(rep_matrix(y, 2, 2),
                                                 rep_matrix(0, 2, 2), sigma);
  target += sum(m);
  y ~ normal(0, 1);
}
