// A user function whose returned size only the callee knows: --O1 inlining
// declares the return variable zero-length (vector[0]) and lets the
// assignment size it, which slot-based lowering must adopt, not reject.
functions {
  vector centered(vector v) {
    vector[rows(v)] out;
    out = v - mean(v);
    return out;
  }
}
data {
  int N;
  vector[N] y;
}
parameters {
  real mu;
}
model {
  target += normal_lpdf(centered(y) | mu, 1);
}
