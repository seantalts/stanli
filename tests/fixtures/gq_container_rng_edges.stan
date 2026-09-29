functions {
  array[] real selected_rng(data int family, data int mode,
                            array[] int counts, vector a, array[] real b) {
    if (family == 0) {
      if (mode == 1) return gamma_rng(a, 1.5);
      if (mode == 2) return gamma_rng(1.5, b);
      return gamma_rng(a, b);
    }
    if (family == 1) {
      if (mode == 1) return binomial_rng(counts, 0.3);
      if (mode == 2) return binomial_rng(7, b);
      return binomial_rng(counts, b);
    }
    if (family == 2) return normal_rng(a, b);
    return beta_binomial_rng(counts, a, b);
  }
}
data {
  int<lower=0> N;
  int<lower=0> M;
  int family;
  int mode;
  int region;
  array[N] int counts;
  array[M] real b;
}
parameters { vector[N] a; }
model { a ~ std_normal(); }
generated quantities {
  real before = normal_rng(0, 1);
  array[N] real draws;
  if (region) {
    int i = 0;
    while (i < 1) {
      draws = selected_rng(family, mode, counts, a, b);
      i += 1;
    }
  } else {
    draws = selected_rng(family, mode, counts, a, b);
  }
  real after = normal_rng(0, 1);
}
