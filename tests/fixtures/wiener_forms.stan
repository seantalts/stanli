data {
  int N;
  vector[N] y;
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
  for (n in 1:N) {
    target += wiener_lpdf(y[n] | a, t0, w, v, sv);
    target += wiener_lpdf(y[n] | a, t0, w, v, sv, 1e-5);
    target += wiener_lpdf(y[n] | a, t0, w, v, sv, sw, st0);
    target += wiener_lpdf(y[n] | a, t0, w, v, sv, sw, st0, 1e-3);
  }
  target += wiener_lpdf(y | rep_vector(a, N), rep_vector(t0, N),
                        rep_vector(w, N), rep_vector(v, N),
                        rep_vector(sv, N));
  y[1] ~ wiener(a, t0, w, v, sv);
  if (v > 0) target += wiener_lpdf(y[2] | a, t0, w, v, sv, sw, st0, 1e-3);
}
