// A partial sum that adds its terms into a local, as brms threading()
// programs do, against the same terms added to target one at a time.
functions {
  real partial_lpmf(array[] int seq, int start, int end, array[] int y,
                    vector x, real a, real b) {
    real ptarget = 0;
    for (n in 1:(end - start + 1)) {
      int nn = n + start - 1;
      if (y[nn] == 0)
        ptarget += log1p_exp(b);
      else
        ptarget += poisson_log_lpmf(y[nn] | a * x[nn]) + log_inv_logit(b);
    }
    return ptarget;
  }
}
data {
  int N;
  array[N] int y;
  vector[N] x;
  int mode;
}
transformed data {
  array[N] int seq = linspaced_int_array(N, 1, N);
}
parameters {
  real a;
  real b;
}
model {
  if (mode == 0) {
    target += reduce_sum(partial_lpmf, seq, 1, y, x, a, b);
  } else if (mode == 1) {
    real acc = 0;
    for (n in 1:N) {
      if (y[n] == 0)
        acc += log1p_exp(b);
      else
        acc += poisson_log_lpmf(y[n] | a * x[n]) + log_inv_logit(b);
    }
    target += acc;
  } else {
    for (n in 1:N) {
      if (y[n] == 0)
        target += log1p_exp(b);
      else
        target += poisson_log_lpmf(y[n] | a * x[n]) + log_inv_logit(b);
    }
  }
}
