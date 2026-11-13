// An overloaded user function in a parameter bound. transform_inits carries
// the bound in the transform it inverts, so that call has to be resolved to
// the overload its argument selects like every other call.
functions {
  real lb(real x) {
    return x - 1;
  }
  real lb(vector x) {
    return x[1] - 1;
  }
}
data {
  int N;
  vector[N] y;
}
parameters {
  real<lower=lb(y)> mu;
  real<lower=lb(0.5)> nu;
}
model {
  target += normal_lpdf(mu | 0, 1);
  target += normal_lpdf(nu | 0, 1);
}
