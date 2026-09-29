parameters {
  real x;
}
model {
  x ~ std_normal();
}
generated quantities {
  int untouched;
  int copied = untouched;
  int assigned;
  real before = assigned;
  if (x > 0) assigned = 7;
  int conditional = assigned;
  assigned = 11;
  int after = assigned;
  array[2] int inside;
  for (i in 1:2) {
    int local;
    inside[i] = local;
  }
  real draw = normal_rng(x, 1);
}
