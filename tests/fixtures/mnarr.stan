// `array[N] vector[K]` data into a vectorized multivariate density.
//
// The interpreter stores every 2-D value with the first index fastest, the
// way it stores a matrix. An array of vectors has to reach the kernel with
// element n contiguous in K, which is where a parameter of the same type
// already sits, so the data path has to repack on the way into the slot.
//
// Getting this wrong permutes the observations against the components. It
// is silent: the model compiles, lp__ stays plausible, and every gradient
// is wrong. No posteriordb model has this shape, so the corpus does not
// cover it. N and K differ here on purpose; a square case hides the
// transpose.
data {
  int N;
  int K;
  array[N] vector[K] y;
  matrix[K, K] Sigma;
}
parameters {
  vector[K] mu;
}
model {
  y ~ multi_normal(mu, Sigma);
}
