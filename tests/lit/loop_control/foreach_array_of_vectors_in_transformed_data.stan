// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"av": [[1, 2, 3], [4, 5, 6]]}
// A foreach loop over an array of vectors runs once per array element:
// size(av) times, not num_elements(av) times.
data {
  array[2] vector[3] av;
}
transformed data {
  real s = 0;
  for (a in av) s += sum(a);
  if (s != 21) reject("s is ", s);
}
parameters {
  real q;
}
model {
  q ~ normal(0, 1);
}
