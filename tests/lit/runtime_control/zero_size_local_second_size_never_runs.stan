// STANLI-LIT: XFAIL
// STANLI-LIT-EXPECT: COMPILE_FAIL runtime-control region: v is given containers of different sizes under a condition that depends on a parameter, which stanli does not support
// CmdStan loads this program: the second branch never runs, so v always
// has size 2. stanli gives v one size for the region and sees a second
// one, so it refuses the model instead of choosing.
parameters {
  real mu;
}
model {
  vector[0] v;
  if (mu > -1e300) v = rep_vector(1, 2);
  if (mu < -1e300) v = rep_vector(2, 3);
  mu ~ normal(0, 1);
  target += sum(v) * mu;
}
