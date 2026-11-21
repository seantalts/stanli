// The taken arm changes the value that its condition reads before it
// returns. Stan tests the condition once. The overload keeps stanc from
// inlining flip, so stanli rewrites its returns itself.
functions {
  vector flip(vector x) {
    vector[2] y = 2 * x;
    if (y[1] < 0) {
      y[1] = -y[1];
      return y;
    } else {
      return 3 * y;
    }
  }
  real flip(real x) {
    return x;
  }
}
parameters {
  real mu;
}
model {
  target += sum(flip([mu, 1]')) + 0.5 * sum(flip([-mu, 1]'));
  mu ~ normal(0, 1);
}
