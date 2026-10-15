functions {
  real partial_lpmf(array[] int seq, int start, int end, vector y, vector x,
                    real a, real b, real s) {
    real ptarget = 0;
    int N = end - start + 1;
    for (n in 1:N) {
      int nn = n + start - 1;
      real r = y[nn] - a * x[nn];
      if (r < b)
        ptarget += normal_lpdf(r | 0, s - 0.01 * nn);
      else
        ptarget += normal_lpdf(r | 0, s - 0.01 * nn) + b;
    }
    return ptarget;
  }
}
data {
  int N;
  vector[N] y;
  vector[N] x;
  int chunk;
  int lo;
  int hi;
}
transformed data {
  array[N] int seq = linspaced_int_array(N, 1, N);
}
parameters {
  real a;
  real b;
  real s;
}
model {
  if (chunk == 0)
    target += reduce_sum(partial_lpmf, seq, 1, y, x, a, b, s);
  else
    target += partial_lpmf(seq[lo:hi] | lo, hi, y, x, a, b, s);
}
