// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 3, "y": [0.5, -0.25, 1.5]}
// The only reference to part_lpdf is the part_lupdf functor.
functions {
  real part_lpdf(array[] real slice, int start, int end, vector mu, real sigma,
                 vector a, vector b, vector c, vector d) {
    return normal_lpdf(slice | mu[start:end], sigma);
  }
  real part_lpdf(array[] real slice, int start, int end, real mu, real sigma) {
    return normal_lpdf(slice | mu, sigma);
  }
}
data {
  int N;
  array[N] real y;
}
parameters {
  real m;
  real<lower=0> s;
}
model {
  vector[N] z = rep_vector(0, N);
  target += reduce_sum(part_lupdf, y, 1, rep_vector(m, N), s, z, z, z, z);
  target += reduce_sum(part_lupdf, y, 1, m, s);
}
