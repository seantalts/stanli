functions {
  tuple(vector, matrix, array[] matrix)
    coef(data array[] int r_idx, int R, int S, vector tau) {
    int P_r = size(r_idx);
    matrix[R, P_r] beta_r;
    array[P_r] matrix[R, S] beta_rs;
    for (p in 1:P_r) {
      beta_r[:, p] = tau[r_idx[p]] * linspaced_vector(R, 1, R);
      beta_rs[p] = tau[r_idx[p]] * rep_matrix(p, R, S);
    }
    return (head(tau, 2), beta_r, beta_rs);
  }
  array[] matrix offsets(int n, int R, int S, vector tau) {
    array[n] matrix[R, S] b;
    for (i in 1:n) {
      b[i] = rep_matrix(tau[i], R, S);
    }
    return b;
  }
}
data {
  int<lower=1> R, S;
  array[2] int<lower=0, upper=3> P_r;
  array[2, max(P_r)] int<lower=0, upper=3> r_idx;
}
parameters {
  vector[3] tau;
}
transformed parameters {
  matrix[R, P_r[1]] beta_r;
  array[P_r[1]] matrix[R, S] beta_rs;
  {
    tuple(vector[2], matrix[R, P_r[1]], array[P_r[1]] matrix[R, S]) c =
      coef(r_idx[1, :P_r[1]], R, S, tau);
    beta_r = c.2;
    if (P_r[1]) {
      beta_rs = c.3;
    }
  }
}
model {
  tuple(vector[2], matrix[R, P_r[2]], array[P_r[2]] matrix[R, S]) c =
    coef(r_idx[2, :P_r[2]], R, S, tau);
  array[P_r[1]] matrix[R, S] none = offsets(P_r[1], R, S, tau);
  array[P_r[2]] matrix[R, S] some = offsets(P_r[2], R, S, tau);
  target += normal_lpdf(tau | 0, 1) + sum(c.1) + sum(c.2) + size(c.3)
            + size(none) + size(beta_rs) + sum(beta_r);
  for (p in 1:P_r[2]) {
    target += -0.01 * (sum(c.3[p]) + sum(some[p]));
  }
}
generated quantities {
  array[P_r[1]] matrix[R, S] empty_draws = offsets(P_r[1], R, S, tau);
  int n_empty = size(empty_draws) + size(beta_rs);
}
