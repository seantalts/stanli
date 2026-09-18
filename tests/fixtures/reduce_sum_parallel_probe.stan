// Developer feasibility fixture: data selects the existing whole-slice
// reduction or one fixed-bound callback. No experimental runtime lowering.
functions {
  real partial_lpdf(array[] real s, int start, int end, data vector x,
                    vector b, real alias_b, data int kind) {
    vector[size(s)] mu = x[start:end] + b[1] + alias_b + 0.001 * sum(b);
    real sigma = exp(b[2]);
    if (kind == 0) return normal_lpdf(s | mu, sigma);
    return student_t_lpdf(s | 2 + sigma, mu, sigma);
  }
}
data {
  int<lower=0> N;
  int<lower=2> P;
  int<lower=0, upper=1> active_slice;
  int<lower=0, upper=1> kind;
  int<lower=0, upper=1> chunk;
  int start;
  int end;
  array[N] real y;
  vector[N] x;
}
parameters {
  vector[P] b;
  array[active_slice ? N : 0] real z;
}
model {
  if (chunk == 0) {
    if (active_slice)
      target += reduce_sum(partial_lpdf, z, 1, x, b, b[1], kind);
    else
      target += reduce_sum(partial_lpdf, y, 1, x, b, b[1], kind);
  } else if (start <= end) {
    if (active_slice)
      target += partial_lpdf(z[start:end] | start, end, x, b, b[1], kind);
    else
      target += partial_lpdf(y[start:end] | start, end, x, b, b[1], kind);
  }
}
