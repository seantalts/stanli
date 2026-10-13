functions {
  matrix refresh(matrix base, vector p, array[,] int setup, int block) {
    matrix[rows(base), cols(base)] out;
    int changed = 0;
    for (r in 1:size(setup)) {
      if (setup[r, 1] == block) {
        out[setup[r, 2], setup[r, 3]] = p[setup[r, 4]];
        changed = 1;
      }
    }
    if (changed) {
      for (i in 1:rows(base))
        for (j in 1:cols(base))
          if (is_nan(out[i, j])) out[i, j] = base[i, j];
      return out;
    } else return base;
  }
}
data {
  int N;
  vector[N] y;
  array[2] int dims;
  array[3, 4] int setup;
  int flag;
}
parameters {
  real mu;
  real<lower=0> sigma;
  vector[2] p;
}
model {
  matrix[2, 2] M = rep_matrix(0, 2, 2);
  matrix[dims[1], dims[2]] P;
  real s = 0;
  for (n in 0:N) {
    int row = n ? n : 1;
    if (n == 0 || flag) P = refresh(P, p, setup, 10);
    if (n == 0) M = refresh(M, p, setup, 1);
    if (n > 0) {
      s = tanh(0.8 * s + M[1, 1] * y[row] + M[2, 2] * 0.5);
      target += normal_lpdf(y[row] | mu + s + rows(P), sigma);
    }
  }
  target += normal_lpdf(mu | 0, 2) + lognormal_lpdf(sigma | 0, 1)
            + normal_lpdf(p | 0, 1);
}
