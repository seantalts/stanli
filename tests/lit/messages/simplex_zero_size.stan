// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL Found dimension size less than one in constrained type declaration (simplex, sum_to_zero_vector, etc.); variable=theta; dimension size expression=K; expression value=0
// STANLI-LIT-DATA: {"K": 0}
// CmdStan rejects the declaration when it reads the data, with Stan Math's
// validate_positive_index message.
data { int K; }
parameters {
  simplex[K] theta;
  real mu;
}
model { mu ~ normal(0, 1); }
