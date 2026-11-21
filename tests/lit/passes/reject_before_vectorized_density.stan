// STANLI-LIT: XFAIL
// STANLI-LIT-EXPECT: Random variable[30] is -1
// STANLI-LIT-DATA: {"N": 40, "y": [0.3, 0.35, 0.4, 0.45, 0.5, 0.55, 0.6, 0.65, 0.7, 0.75, 0.8, 0.85, 0.9, 0.95, 1.0, 1.05, 1.1, 1.15, 1.2, 1.25, 1.3, 1.35, 1.4, 1.45, 1.5, 1.55, 1.6, 1.65, 1.7, -1.0, 1.8, 1.85, 1.9, 1.95, 2.0, 2.05, 2.1, 2.15, 2.2, 2.25]}
// CmdStan stops at the reject when n is 6 ("reject reached at 6"). The
// stanli compiler's loop vectorizer moves the density out of the loop, ahead
// of the reject, so the density's error about y[30] is reported instead.
data {
  int N;
  vector[N] y;
}
parameters {
  real mu;
  real<lower=0> s;
}
model {
  for (n in 1:N) {
    if (n == 6) reject("reject reached at ", n);
    y[n] ~ lognormal(mu, s);
  }
}
