// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 40, "y": [0, 2, 0, 4, 0, 0, 0, 4, 0, 2, 0, 4, 0, 2, 0, 0, 0, 2, 0, 4, 0, 2, 0, 4, 0, 0, 0, 4, 0, 2, 0, 4, 0, 2, 0, 0, 0, 2, 0, 4]}
// STANLI-LIT-DUMP: log_prob:cse
// STANLI-LIT-CHECK-NOT: = INDEX
// STANLI-LIT-CHECK: = BERNOULLI_LPMF.v=0x01 s{{[0-9]+}}[1] idata=[1]
// STANLI-LIT-CHECK-NOT: = BERNOULLI_LPMF.v=0x01 s{{[0-9]+}}[1] idata=[1]
// STANLI-LIT-CHECK-NOT: = INDEX
// The zeros of a hurdle model each add the same term, and each passes an
// element of mu that its branch never reads.
functions {
  real hurdle_poisson_log_lpmf(int y, real eta, real hu) {
    if (y == 0) {
      return bernoulli_lpmf(1 | hu);
    } else {
      return bernoulli_lpmf(0 | hu) + poisson_log_lpmf(y | eta)
             - log1m_exp(-exp(eta));
    }
  }
}
data {
  int<lower=1> N;
  array[N] int y;
}
parameters {
  real eta;
  real<lower=0, upper=1> hu;
}
model {
  vector[N] mu = rep_vector(0.0, N);
  mu += eta;
  for (n in 1 : N) {
    target += hurdle_poisson_log_lpmf(y[n] | mu[n], hu);
  }
}
