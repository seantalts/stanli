// Vector fma from partial evaluation: --O1 rewrites `k .* t + c` into
// fma(k, t, c) with all-vector arguments (prophet's linear_trend), which
// the reader desugars elementwise.
data {
  int N;
  vector[N] t;
}
parameters {
  vector[N] k;
  vector[N] c;
}
model {
  target += normal_lpdf(k .* t + c | 0, 1);
}
