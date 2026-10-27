data {
  int N;
  vector[N] z;
}
parameters {
  real mu;
}
model {
  vector[3] v = rep_vector(0, 3);
  for (n in 1:N) {
    if (mu > 100) target += -1;
    v[1] = z[n];
    target += normal_lpdf(v | mu, 1);
    v[2] = z[n] * 2;
  }
}
