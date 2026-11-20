// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL Found dimension size less than one in constrained type declaration (simplex, sum_to_zero_vector, etc.); variable=z; dimension size expression=K; expression value=0
// STANLI-LIT-DATA: {"K": 0}
data { int K; }
parameters {
  array[2] sum_to_zero_vector[K] z;
  real mu;
}
model { mu ~ normal(0, 1); }
