// A foreach loop over an array of vectors runs once per array element:
// size(av) times, not num_elements(av) times.
data {
  array[2] vector[3] av;
}
parameters {
  real q;
}
model {
  q ~ normal(0, 1);
}
generated quantities {
  real s = 0;
  for (a in av) s += sum(a);
}
