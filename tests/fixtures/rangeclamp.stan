// Ranges whose realized bounds make them empty (hi < lo) are empty under
// CmdStan's rvalue semantics, whatever the endpoints, so the graph paths
// must emit zero-length slices rather than reject the program or emit a
// negative length. K, lo, and hi are data: emptiness is data-dependent.
data {
  int K;
  int lo;
  int hi;
  vector[4] Y;
  matrix[4, 2] Zm;
}
transformed data {
  array[3] real xs = {1.0, 2.0, 3.0};
}
parameters {
  real mu;
}
model {
  target += normal_lpdf(sum(xs[1:K]) | mu, 1);
  target += normal_lpdf(Y[lo:hi] | mu, 1);
  target += normal_lpdf(sum(Zm[1:K, 1]) | mu, 1);
  target += normal_lpdf(mu | 0, 2);
}
