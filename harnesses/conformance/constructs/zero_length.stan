data {
  int<lower=0> N;
  vector[N] x;
}
parameters {
  real theta;
}
transformed parameters {
  vector[N] copied = x;
}
model {
  target += theta + sum(copied);
}
generated quantities {
  array[N] real generated_values;
}
