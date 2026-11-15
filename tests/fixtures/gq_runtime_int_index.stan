functions {
  real tf(real x, int t) {
    real y = x;
    if (t == 1) y = exp(y);
    if (t == 2) y = square(y);
    return y;
  }
}
data {
  int N;
  array[N, 2] int ms;
  matrix[N, 2] mv;
}
parameters {
  real mu;
}
model {
  mu ~ normal(0, 1);
}
generated quantities {
  real by_int;
  real by_int_element;
  real by_real_element;
  vector[N] by_store = rep_vector(0, N);
  array[N] real by_size;
  {
    int p = 1;
    while (p < N && mu > mv[p, 1]) p += 1;
    by_int = tf(mu, p);
    by_int_element = tf(mu, ms[p, 2]);
    by_real_element = mu * mv[p, 2];
    by_store[p] = mu;
  }
  for (k in 1:N) {
    int found = 0;
    int q;
    while (!found) {
      for (r in 1:N) {
        if (ms[r, 1] == k) {
          q = r;
          found = 1;
        }
      }
    }
    vector[ms[q, 2]] v = rep_vector(mu, ms[q, 2]);
    by_size[k] = sum(v);
  }
}
