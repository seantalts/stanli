// log_prob fails to lower and write_array would fall back to the interpreter.
data { array[2] int<lower=0> counts; }
parameters { row_vector[2] a; row_vector[3] b; }
model { target += sum(append_row(a, b)); }
generated quantities {
  real nb = 0;
  int i = 1;
  while (i <= 2 && a[1] > -1e9) {
    nb += neg_binomial_2_lpmf(counts[i] | 2.0, 1.5);
    i += 1;
  }
}

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL append_row column mismatch
// STANLI-LIT-DATA: {"counts": [1, 2]}
