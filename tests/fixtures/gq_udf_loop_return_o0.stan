functions {
  real first_big(vector v, real cut) {
    for (n in 1:rows(v)) {
      if (v[n] > cut) return v[n];
    }
    return 0;
  }
  int first_row(matrix m, real cut) {
    for (i in 1:rows(m)) {
      for (j in 1:cols(m)) {
        if (m[i, j] > cut) return i;
      }
    }
    return 0;
  }
}
data {
  int N;
}
parameters {
  vector<lower=0>[N] mu;
}
model {
  mu ~ exponential(1);
}
generated quantities {
  real big = first_big(mu, 1);
  int row = first_row(to_matrix(mu, 2, N %/% 2), 1.2);
  int steps = 0;
  int hit = 0;
  for (n in 1:N) {
    if (mu[n] > 1.01) hit = 1;
    if (hit) break;
    steps += 1;
  }
}
