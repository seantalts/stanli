// write_array cannot be lowered (a density evaluated in a loop whose bound
// depends on a draw), so it is handed to the interpreter; log_prob lowers
// cleanly.
data { array[2] int<lower=0> counts; }
parameters { vector[2] mu; }
model { mu ~ normal(0, 1); }
generated quantities {
  real nb = 0;
  int i = 1;
  while (i <= 2 && mu[1] > -1e9) {
    nb += neg_binomial_2_lpmf(counts[i] | 2.0, 1.5);
    i += 1;
  }
}

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"counts": [1, 2]}
