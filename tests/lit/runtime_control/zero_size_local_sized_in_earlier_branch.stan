// STANLI-LIT: XFAIL
// STANLI-LIT-EXPECT: COMPILE_FAIL runtime-control region: v is given containers of different sizes
// As zero_size_local_takes_two_sizes, with the two sizes in separate
// branches. The first branch chooses v's size, so the second one cannot
// reject a different size at run time.
parameters {
  real mu;
}
model {
  vector[0] v;
  if (mu < 0) v = rep_vector(1, 2);
  if (mu >= 0) v = rep_vector(2, 3);
  mu ~ normal(0, 1);
  target += sum(v) * mu;
}
