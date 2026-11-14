functions {
  void prior_lp(vector x, vector m, vector s) {
    for (i in 1:rows(x)) {
      real wide = 2 * s[i];
      x[i] ~ normal(m[i], s[i]);
      x[i] ~ normal(m[i] + 1, wide);
    }
  }
  real tail_lp(real x, vector s) {
    x ~ normal(0, s[2]);
    return 0;
  }
}
data {
  vector[2] m;
  vector<lower=0>[2] s;
}
parameters {
  real tau;
  vector[2] x;
}
model {
  prior_lp(x, m, s);
  if (tau > 0) target += tail_lp(tau, s);
}
