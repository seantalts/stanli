// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 6, "J": 2, "j": [1, 2, 1, 2, 2, 1], "x": [0.5, -1.0, 0.25, 2.0, -0.5, 1.5], "y": [0.1, 0.4, -0.3, 1.2, 0.0, 0.7]}
// STANLI-LIT-DUMP: log_prob:lower
// STANLI-LIT-CHECK: ops=6
// STANLI-LIT-CHECK: s{{[0-9]+}}[6] = GATHER s{{[0-9]+}}[2,P] idata=[0,1,0,1,1,0]
// STANLI-LIT-CHECK: s{{[0-9]+}}[6] = SET_SLICE s{{[0-9]+}}[6] s{{[0-9]+}}[6] idata=[0]
// STANLI-LIT-CHECK-NEXT: s{{[0-9]+}}[1] = NORMAL_LPDF.v=0x06 s{{[0-9]+}}[6] s{{[0-9]+}}[6] s{{[0-9]+}}[1]
// STANLI-LIT-CHECK-NOT: LOOP
// stanc3 turns this loop into two vector statements, because every mention
// of mu in it is the element mu[n]. Lowering sees no loop, so the program
// has six ops at any N and never meets the unroll ceiling. The ranges
// mu[1:N] and y[1:N] cover the whole vector and emit no SLICE.
data {
  int<lower=1> N;
  int<lower=1> J;
  array[N] int<lower=1, upper=J> j;
  vector[N] x;
  vector[N] y;
}
parameters {
  vector[J] a;
  real b;
  real<lower=0> sigma;
}
model {
  vector[N] mu;
  for (n in 1 : N) {
    mu[n] = a[j[n]] + b * x[n];
    target += normal_lpdf(y[n] | mu[n], sigma);
  }
}
