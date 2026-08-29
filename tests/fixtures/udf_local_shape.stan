functions {
  matrix square_copy(matrix x) {
    int d = rows(x);
    matrix[d, d] out;
    out = x;
    for (i in 1:d) {
      if (x[i, i] > 0) {
        out[i, i] = x[i, i] + 1;
      }
    }
    return out;
  }
}
data {
  int<lower=1> D;
}
parameters {
  matrix[D, D] x;
}
model {
  to_vector(x) ~ normal(0, 1);
}
generated quantities {
  matrix[D, D] copied = square_copy(x);
}
