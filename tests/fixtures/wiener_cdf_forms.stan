data {
  int N;
  vector[N] y;
}
transformed data {
  real td = wiener_lcdf_unnorm(y[1], 1.2, 0.05, 0.5, 0.3)
            + wiener_lccdf_unnorm(y[2], 1.2, 0.05, 0.5, 0.3, 1e-5)
            + wiener_lcdf_unnorm(y[1], 1.2, 0.05, 0.5, 0.3, 0.4, 0.1, 0.02)
            + wiener_lccdf_unnorm(y[2], 1.2, 0.05, 0.5, 0.3, 0.4, 0.1, 0.02,
                                  1e-6)
            + wiener_lpdf(y[3] | 1.2, 0.05, 0.5, 0.3, 0.4)
            + wiener_lpdf(y[3] | 1.2, 0.05, 0.5, 0.3, 0.4, 0.1, 0.02);
}
parameters {
  real a;
  real t0;
  real w;
  real v;
  real sv;
  real sw;
  real st0;
}
model {
  target += td;
  for (n in 1:N) {
    target += wiener_lcdf_unnorm(y[n], a, t0, w, v);
    target += wiener_lcdf_unnorm(y[n], a, t0, w, v, 1e-5);
    target += wiener_lccdf_unnorm(y[n], a, t0, w, v);
    target += wiener_lccdf_unnorm(y[n], a, t0, w, v, 1e-5);
    target += wiener_lcdf_unnorm(y[n], a, t0, w, v, sv, sw, st0);
    target += wiener_lccdf_unnorm(y[n], a, t0, w, v, sv, sw, st0, 1e-6);
  }
  target += wiener_lcdf_unnorm(y, rep_vector(a, N), rep_vector(t0, N),
                               rep_vector(w, N), rep_vector(v, N));
  target += wiener_lccdf_unnorm(y, rep_vector(a, N), rep_vector(t0, N),
                                rep_vector(w, N), rep_vector(v, N));
  target += wiener_lcdf_unnorm(y, rep_vector(a, N), rep_vector(t0, N),
                               rep_vector(w, N), rep_vector(v, N),
                               rep_vector(sv, N), rep_vector(sw, N),
                               rep_vector(st0, N));
  if (v > 0) {
    target += wiener_lccdf_unnorm(y[2], a, t0, w, v, sv, sw, st0, 1e-6);
    target += wiener_lcdf_unnorm(y[2], a, t0, w, v, 1e-5);
  }
}
