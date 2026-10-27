data {
  int<lower=1> R;
  int<lower=1> C;
  int<lower=0> NZ;
  array[NZ] int v;
  array[R + 1] int u;
  vector[NZ] wd;
  vector[C] bd;
  vector[R] y;
}
parameters {
  vector[NZ] wp;
  vector[C] bp;
}
model {
  target += normal_lpdf(y | csr_matrix_times_vector(R, C, wd, v, u, bp), 1);
  target += normal_lpdf(y | csr_matrix_times_vector(R, C, wp, v, u, bd), 1);
  target += normal_lpdf(y | csr_matrix_times_vector(R, C, wp, v, u, bp), 1);
  target += normal_lpdf(y | csr_matrix_times_vector(R, C, wd, v, u, bd), 1);
}
