// STANLI-LIT: XFAIL
// STANLI-LIT-EXPECT: COMPILE_FAIL runtime-control region: v is given containers of different sizes
// Stan gives a zero-size local the size of its first assignment, so each
// arm here gives v a different size and CmdStan accepts both. stanli
// cannot give v a size that depends on a parameter, so it refuses the model.
parameters {
  real mu;
}
model {
  vector[0] v;
  if (mu < 0) v = rep_vector(1, 2);
  else v = rep_vector(2, 3);
  mu ~ normal(0, 1);
  target += sum(v) * mu;
}
