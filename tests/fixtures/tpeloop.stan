// `target += <container>` under a loop, so the container increment is
// reached once per iteration rather than once per model.
data {
  int<lower=0> N;
}
transformed data {
  real half = 0.5;
}
parameters {
  array[3] vector[2] f;
  real z;
}
model {
  for (n in 1 : N) {
    target += f[n];
  }
  target += z * half;
}
generated quantities {
  real s = sum(f[1]) + half;
}
