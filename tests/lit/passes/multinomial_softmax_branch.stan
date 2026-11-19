// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 2, "K": 3, "Y": [[1, 0, 4], [2, 3, 0]]}
// STANLI-LIT-DUMP: bounded_log_prob:lower
// A branch on a parameter runs as a register program, which passes the
// multinomial coefficient as a constant register.
// STANLI-LIT-CHECK-NOT: OP_SOFTMAX(
// STANLI-LIT-CHECK: CALL      OP_MULTINOMIAL_SOFTMAX_LPMF(r{{[0-9]+}}[len 3], r{{[0-9]+}}[len 1])
// STANLI-LIT-CHECK: CALL      OP_MULTINOMIAL_SOFTMAX_LPMF(r{{[0-9]+}}[len 3], r{{[0-9]+}}[len 1])
data { int N; int K; array[N, K] int Y; }
parameters { array[N] vector[K] mu; real s; }
model {
  s ~ normal(0, 1);
  for (n in 1:N) {
    mu[n] ~ normal(0, 1);
    if (s > -100) {
      target += multinomial_lpmf(Y[n] | softmax(mu[n]));
      target += multinomial_lpmf(Y[n] | softmax(mu[n] * s));
    }
  }
}
