// STANLI-LIT: XFAIL
// STANLI-LIT-EXPECT: not known at compile time
// The while form of carried_int_chain_sizes_vector: a sizes a vector, and
// the loop's guard reads a parameter. a has no bounded range, so the
// retained loop cannot hold v, and the register program needs a's value
// at compile time. A range taken after one step of the analysis held a at
// 1 and gave a wrong value instead of this error.
parameters {
  real<lower=0.5, upper=9.5> theta;
}
model {
  real s = 0;
  for (g in 1:40) {
    int a = 1;
    int b = 1;
    real t = 0;
    while (t < theta) {
      vector[a] v = rep_vector(1.0, a);
      s += sum(v);
      a = b;
      b += 1;
      t += 1;
    }
  }
  target += -s;
}
