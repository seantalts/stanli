data {
  int N;
  int T;
  array[T] int flag;
  array[N] real z;
}
parameters {
  real mu;
}
model {
  vector[3] v = rep_vector(0, 3);
  vector[3] w = rep_vector(1, 3);
  for (n in 1:N) {
    v[1] = z[n];
    target += normal_lpdf(v | mu, 1);
    for (t in 1:T) {
      if (flag[t] == 1) w = v;
    }
  }
  target += normal_lpdf(w | mu, 1);
}
