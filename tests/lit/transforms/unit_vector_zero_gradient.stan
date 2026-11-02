// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// Stan Math's reverse-mode unit_vector_constrain does not check the norm,
// so CmdStan's gradient at a zero vector is NaN rather than an error; its
// write_array, which runs the double overload, does throw.
parameters {
  vector[2] p;
}
transformed parameters {
  vector[2] u = unit_vector_constrain(p - p);
}
model {
  p ~ normal(0, 1);
}
