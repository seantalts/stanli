// STANLI-LIT: XFAIL
// STANLI-LIT-EXPECT: COMPILE_FAIL runtime-control region: v is given containers of different sizes under a condition that depends on a parameter, which stanli does not support
// v takes size 2 and is then assigned size 3, which CmdStan rejects at run
// time: "vector assign rows: assigning variable v (2) and right hand side
// rows (3) must match in size". Inside one region stanli refuses the model
// while loading it instead.
parameters {
  real mu;
}
model {
  vector[0] v;
  if (mu > -1e300) {
    if (mu > -1e299) v = rep_vector(1, 2);
    v = rep_vector(2, 3);
  }
  mu ~ normal(0, 1);
  target += sum(v) * mu;
}
