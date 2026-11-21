// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: vector assign rows: assigning variable v (2) and right hand side rows (3) must match in size
// v takes size 2 before the branch runs, so the branch's assignment of
// size 3 is an error when it runs, in CmdStan and in stanli.
parameters {
  real mu;
}
model {
  vector[0] v;
  v = rep_vector(1, 2);
  if (mu > -100) v = rep_vector(2, 3);
  mu ~ normal(0, 1);
  target += sum(v) * mu;
}
