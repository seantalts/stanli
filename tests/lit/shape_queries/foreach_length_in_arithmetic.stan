// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// stanc3's vectorize_loops turns this loop into
// `if (FnLength(tmp) >= 1) target += FnLength(tmp) * logistic_lupdf(...)`,
// where tmp is the loop's unsized temporary holding a parameter row vector.
parameters {
  real mu;
}
model {
  for (e in rep_row_vector(mu, 2)) {
    mu ~ logistic(0.5, 1);
  }
}
