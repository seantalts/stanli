functions {
  matrix spread(data array[] int sizes, matrix w) {
    int R = size(sizes), total = sum(sizes), C = cols(w) + 1, wide = C > 1,
        at = 0;
    matrix[wide * total, C] out = rep_matrix(0, wide * total, C);
    for (r in 1:R) {
      int n = sizes[r];
      for (k in 1:n) {
        out[at + k] = append_col(w[r], 1.0 * k);
      }
      at += n;
    }
    return out;
  }
}
data {
  int<lower=1> N;
  int<lower=1> R;
  array[R] int<lower=1> sizes;
}
parameters {
  real mu;
}
model {
  mu ~ normal(0, 1);
}
generated quantities {
  real fit = 0;
  for (n in 1:N) {
    matrix[R, 2] w = rep_matrix(mu * n, R, 2);
    fit += sum(spread(sizes, w));
  }
}
