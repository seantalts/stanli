// The model block and generated quantities both name `x` at different
// widths. The CSV carries only the generated-quantities one.
parameters {
  real mu;
}
model {
  vector[3] x = rep_vector(mu, 3);
  for (i in 1 : 2) {
    x[i] ~ std_normal();
  }
}
generated quantities {
  vector[2] x;
  x[1] = 10 * mu;
  x[2] = 20 * mu;
}
