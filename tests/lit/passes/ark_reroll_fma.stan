// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"K": 5, "T": 20, "y": [0.0, 0.744218, 1.18545, 1.163209, 0.734988, 0.149217, -0.271576, -0.282453, 0.168733, 0.916814, 0.656987, 1.088168, 1.054599, 0.619098, 0.033521, -0.379696, -0.379178, 0.081863, 0.833623, 1.56957]}
// STANLI-LIT-DUMP: log_prob:reroll
// STANLI-LIT-CHECK: s{{[0-9]+}}[1] = INDEX s{{[0-9]+}}[5,P] idata=[0]
// STANLI-LIT-CHECK-NEXT: s{{[0-9]+}}[15] = FMA s{{[0-9]+}}[1] s{{[0-9]+}}[15] s{{[0-9]+}}[1,P]
// STANLI-LIT-CHECK: s{{[0-9]+}}[1] = INDEX s{{[0-9]+}}[5,P] idata=[4]
// STANLI-LIT-CHECK-NEXT: s{{[0-9]+}}[15] = FMA s{{[0-9]+}}[1] s{{[0-9]+}}[15] s{{[0-9]+}}[15]
// STANLI-LIT-CHECK-NEXT: s{{[0-9]+}}[1] = NORMAL_LPDF.v=0x86 s{{[0-9]+}}[15] s{{[0-9]+}}[15] s{{[0-9]+}}[1]
data {
  int<lower=0> K;
  int<lower=0> T;
  array[T] real y;
}
parameters {
  real alpha;
  array[K] real beta;
  real<lower=0> sigma;
}
model {
  alpha ~ normal(0, 10);
  beta ~ normal(0, 10);
  sigma ~ cauchy(0, 2.5);
  for (t in (K + 1) : T) {
    real mu = alpha;
    for (k in 1 : K) {
      mu = mu + beta[k] * y[t - k];
    }
    y[t] ~ normal(mu, sigma);
  }
}
