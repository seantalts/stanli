// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: reject reached at 6
// STANLI-LIT-DATA: {"N": 40, "g": [1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2], "y": [0.3, 0.35, 0.4, 0.45, 0.5, 0.55, 0.6, 0.65, 0.7, 0.75, 0.8, 0.85, 0.9, 0.95, 1.0, 1.05, 1.1, 1.15, 1.2, 1.25, 1.3, 1.35, 1.4, 1.45, 1.5, 1.55, 1.6, 1.65, 1.7, -1.0, 1.8, 1.85, 1.9, 1.95, 2.0, 2.05, 2.1, 2.15, 2.2, 2.25]}
// The reject depends on a parameter, so it lowers into a compiled region
// rather than a bare reject op. The lanes after it must still stay behind
// it, as in CmdStan, which stops with the reject's message.
data {
  int N;
  array[N] int g;
  vector[N] y;
}
parameters {
  real mu1;
  real mu2;
  real<lower=0> s;
}
model {
  for (n in 1:N) {
    if (n == 6 && mu1 < 100) reject("reject reached at ", n);
    if (g[n] == 1) y[n] ~ lognormal(mu1, s);
    else y[n] ~ lognormal(mu2, s);
  }
}
