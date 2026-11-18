data {
  int N;
  matrix[N, N] X;
  matrix[3, 4] S;
}
transformed data {
  tuple(matrix[N, N], vector[N], matrix[N, N]) usv = svd(X);
  tuple(vector[5], array[5] int, array[4] int) csr = csr_extract(S);
}
parameters {
  vector[N] theta;
}
model {
  tuple(matrix[N, N], vector[N], matrix[N, N]) d
      = svd(X * diag_matrix(exp(theta)));
  tuple(matrix[N, 2], vector[2], matrix[2, 2]) e = svd(X[ : , 1:2] * exp(theta[1]));
  real s = 0;
  int k = 0;
  while (s < theta[1] ^ 2 + 1 && k < 3) {
    tuple(matrix[N, N], vector[N]) ev
        = eigendecompose_sym(crossprod(X) * exp(theta[2]));
    tuple(matrix[N, 2], matrix[2, 2]) qr2 = qr_thin(X[ : , 1:2] * theta[3]);
    s += ev.2[N] + 0.1 * qr2.2[2, 2];
    k += 1;
  }
  theta ~ normal(0, 1);
  target += -0.5 * dot_self(d.2 - usv.2);
  target += sum(d.1[ : , 1] .* d.3[ : , 1]);
  target += 0.1 * sum(e.2) + 0.01 * sum(e.1) + 0.01 * sum(e.3);
  target += 0.01 * sum(csr.1) * sum(theta) + 0.001 * s;
}
generated quantities {
  matrix[N, N] U = usv.1;
  vector[N] sv = usv.2;
  matrix[N, N] V = usv.3;
  array[5] int v = csr.2;
  array[4] int u = csr.3;
  tuple(vector[5], array[5] int, array[4] int) again = csr_extract(S * 2);
  vector[N] sv_theta = svd(diag_matrix(theta)).2;
  tuple(matrix[N, N], matrix[N, N]) f = qr(X * theta[1]);
}
