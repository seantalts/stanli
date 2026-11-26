data { int N; vector[N] y; int G; array[G] int len; array[G] int start; }
parameters { real a; real<lower=0> s; }
model {
  vector[N] mu = exp(rep_vector(a, N));
  for (g in 1:G) {
    for (t in 1:10) {
      int n = start[g] + t - 1;
      if (y[n] > mu[n]) target += normal_lpdf(y[n] | mu[n], s);
      else target += normal_lpdf(y[n] | mu[n], 2 * s);
    }
  }
}
