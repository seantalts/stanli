parameters { real x; }
model { x ~ std_normal(); }
generated quantities {
  real total = 0;
  int i = 0;
  while (i < 2) {
    int n = 1 + poisson_rng(exp(x));
    vector[n] local = rep_vector(x, n);
    total += sum(local);
    i += 1;
  }
}
