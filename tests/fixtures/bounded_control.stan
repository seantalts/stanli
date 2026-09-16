data {
  int<lower=0> N;
  int<lower=0, upper=1> mode;
  array[N] int<lower=0, upper=4> counts;
}
transformed data {
  real shift = normal_rng(0, 1);
  print("prepared once");
}
parameters { real x; }
model {
  real acc = 0;
  for (i in 1:N) {
    vector[4] tmp = rep_vector(x, 4);
    int k = 1;
    if (mode == 0) {
      while (k <= counts[i]) {
        tmp[k] = tmp[k] + x;
        acc += tmp[k];
        k += 1;
      }
    } else {
      while (k <= 4 && x > k) {
        acc += x;
        k += 1;
      }
    }
  }
  target += normal_lpdf(acc | shift, 3) + normal_lpdf(x | 0, 1);
}
generated quantities { real z = x + shift; }
