data { int N; vector[N] y; int G; array[G] int len; array[G] int start; }
parameters { real a; real<lower=0> s; }
model {
  vector[N] mu = exp(rep_vector(a, N));
  for (t in 1:len[1]) {
    int n = start[1] + t - 1;
    if (y[n] > mu[n]) target += normal_lpdf(y[n] | mu[n], s);
    else target += normal_lpdf(y[n] | mu[n], 2 * s);
  }
  for (t in 1:len[2]) {
    int n = start[2] + t - 1;
    if (y[n] > mu[n]) target += normal_lpdf(y[n] | mu[n], s);
    else target += normal_lpdf(y[n] | mu[n], 2 * s);
  }
  for (t in 1:len[3]) {
    int n = start[3] + t - 1;
    if (y[n] > mu[n]) target += normal_lpdf(y[n] | mu[n], s);
    else target += normal_lpdf(y[n] | mu[n], 2 * s);
  }
  for (t in 1:len[4]) {
    int n = start[4] + t - 1;
    if (y[n] > mu[n]) target += normal_lpdf(y[n] | mu[n], s);
    else target += normal_lpdf(y[n] | mu[n], 2 * s);
  }
}
