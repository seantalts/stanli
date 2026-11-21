// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL stanli does not support a local whose size depends on a branch on parameters: v was not assigned on this path
// The branch does not run at the check point, so in Stan v stays empty and
// its sum is 0; CmdStan accepts the point. stanli gives v the branch's
// size, so it stops the evaluation instead of reading values that v does
// not have.
parameters {
  real mu;
}
model {
  vector[0] v;
  if (mu < 0) v = rep_vector(2, 3);
  if (is_nan(sum(v))) reject("the sum of v is NaN");
  mu ~ normal(0, 1);
  target += sum(v);
}
