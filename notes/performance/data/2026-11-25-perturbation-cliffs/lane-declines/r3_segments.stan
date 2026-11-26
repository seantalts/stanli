// Sub-cause 3: one vectorized statement per group over a contiguous segment.
// Each lane mixes a per-group scalar (alpha[j]) with a width-len row.
data {
  int<lower=1> J;
  int<lower=1> N;
  array[J] int start;
  array[J] int len;
  vector[N] x;
  vector[N] y;
}
parameters {
  vector[J] alpha;
  real beta;
  real<lower=0> sigma;
}
model {
  alpha ~ normal(0, 1);
  for (j in 1:J)
    segment(y, start[j], len[j]) ~ normal(alpha[j] + beta * segment(x, start[j], len[j]), sigma);
}
