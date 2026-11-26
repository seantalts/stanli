// Sub-cause 7: one probability vector shared by every row's categorical.
data {
  int<lower=1> N;
  array[N] int<lower=1, upper=3> R;
}
parameters {
  simplex[3] p;
}
model {
  for (i in 1:N) R[i] ~ categorical(p);
}
