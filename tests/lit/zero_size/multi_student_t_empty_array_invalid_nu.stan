// STANLI-LIT: XFAIL
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"nu": -1}
// Stan Math checks nu before it returns 0 for an empty array of outcomes.
// Lowering folds the call to 0 first (DensityCallPlan::empty_result).
data {
  array[0] vector[2] y;
  real nu;
}
parameters {
  vector[2] mu;
  cov_matrix[2] S;
}
model {
  y ~ multi_student_t(nu, mu, S);
  mu ~ normal(0, 1);
}
