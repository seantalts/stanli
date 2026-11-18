functions {
  tuple(vector, matrix, matrix, array[] matrix, array[] matrix)
  coef_cont(vector b, matrix B, matrix Br, array[] matrix Bs,
            array[] matrix Bt, real scale) {
    array[size(Bt)] matrix[rows(Bt[1]), cols(Bt[1])] Bt_scaled;
    for (i in 1:size(Bt)) Bt_scaled[i] = scale * Bt[i];
    return (b, B, scale * Br, Bs, Bt_scaled);
  }
}
data {
  int R;
  int S;
  int MS;
  array[1] int P;
  array[1] int P_r;
  vector[R] y;
  array[R] tuple(real, array[2] int) obs;
  tuple(int, vector[S]) design;
}
parameters {
  vector[P[1]] b;
  matrix[P[1], MS * S] B;
  matrix[R, P_r[1]] Br;
  array[MS * P_r[1]] matrix[R, S] Bs;
  array[P_r[1] ? R : 1] matrix[P[1], S] Bt;
  tuple(real, array[2] real<lower=0>) scales;
}
model {
  tuple(vector[P[1]], matrix[P[1], MS * S], matrix[R, P_r[1]],
        array[MS * P_r[1]] matrix[R, S], array[P_r[1] ? R : 1] matrix[P[1], S])
    coef = coef_cont(b, B, Br, Bs, Bt, scales.2[1]);
  target += normal_lpdf(coef.1 | 0, 1);
  target += normal_lpdf(to_vector(coef.2) | 0, 1);
  target += normal_lpdf(to_vector(coef.3) | 0, 1);
  for (i in 1:size(coef.4))
    target += normal_lpdf(to_vector(coef.4[i]) | design.2[1], 1);
  for (i in 1:size(coef.5))
    target += normal_lpdf(to_vector(coef.5[i]) | 0, scales.2[2]);
  scales.1 ~ normal(0, 1);
  scales.2 ~ lognormal(0, 1);
  for (r in 1:R)
    y[r] ~ normal(coef.3[r, 1] + scales.1 * obs[r].1, 1 + obs[r].2[design.1]);
}
generated quantities {
  tuple(real, vector[P[1]]) summary = (scales.1 * 2, b);
  array[R] tuple(real, int) fitted;
  for (r in 1:R)
    fitted[r] = (Br[r, 1] + obs[r].1, obs[r].2[1] + design.1);
}
