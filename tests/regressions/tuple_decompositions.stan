data {
  int N;
  matrix[N, N] X;
}
transformed data {
  tuple(matrix[N, N], vector[N]) data_eigen = eigendecompose_sym(crossprod(X));
}
parameters {
  matrix[N, N] A;
}
model {
  matrix[N, N] S = crossprod(A) + diag_matrix(rep_vector(1, N));
  tuple(matrix[N, N], vector[N]) eigen = eigendecompose_sym(S);
  matrix[N, N] Q;
  matrix[N, N] R;
  (Q, R) = qr_thin(A);
  tuple(matrix[N, N], matrix[N, N]) full = qr(A + X);
  target += -0.5 * sum(log(eigen.2)) + 0.1 * sum(eigen.1[, N]);
  target += 0.01 * sum(diagonal(R)) + 0.01 * sum(Q[1]);
  target += 0.01 * sum(full.2[1]) + 0.01 * sum(full.1[, 1]);
  to_vector(A) ~ normal(data_eigen.2[N] / 10, 1);
}
generated quantities {
  tuple(matrix[N, N], matrix[N, N]) decomposition = qr_thin(A);
  vector[N] eigenvalues = eigendecompose_sym(crossprod(A)).2;
}
