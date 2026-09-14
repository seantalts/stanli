parameters { real x; }
model { x ~ std_normal(); }
generated quantities {
  real value = 0;
  vector[2] lookup = [1, 2]';
  int i = 0;
  while (i < 1) {
    int k = 3 + bernoulli_rng(0.5);
    value = lookup[k];
    i += 1;
  }
}
