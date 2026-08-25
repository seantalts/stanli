// A bare generated-quantities container with only one element assigned.
// CmdStan leaves the rest NaN; the graph path used to zero-fill them.
parameters {
  real mu;
}
model {
  mu ~ std_normal();
}
generated quantities {
  vector[3] q;
  q[1] = 5.0;
}
