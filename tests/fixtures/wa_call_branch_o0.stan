functions {
  real mean_surv(real h, vector eta) {
    int n = num_elements(eta);
    vector[n] log_s;
    for (i in 1:n) {
      if (h == 0)
        log_s[i] = 0;
      else
        log_s[i] = -h * exp(eta[i]);
    }
    return log_sum_exp(log_s) - log(n);
  }
}
data {
  int N;
  int K;
  vector[K] x;
  matrix[N, 2] B;
}
parameters {
  vector[2] b;
  real mu;
}
model {
  b ~ normal(0, 1);
  mu ~ normal(0, 1);
}
generated quantities {
  array[N] real s;
  {
    vector[K] eta = mu + x;
    for (p in 1:N) {
      real h = dot_product(B[p], b);
      s[p] = mean_surv(h, eta);
    }
  }
}
