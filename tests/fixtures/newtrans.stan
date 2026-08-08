// Every parameter transform added for CmdStan parity, in one model, so
// CI has a guard where harnesses/transform_sweep.py (which needs a
// CmdStan checkout) cannot run. The target touches every element of
// every constrained value, so a transform that is wrong in a component
// nothing reads still shows up in the gradient.
data {
  real m;
  real<lower=0.5> s;
}
parameters {
  real<offset=m, multiplier=s> a;
  vector<offset=m, multiplier=s>[3] b;
  real<offset=m> c;
  real<multiplier=s> d;
  vector[3] mu_p;
  vector<lower=0>[3] sg_p;
  vector<offset=mu_p, multiplier=sg_p>[3] e;
  unit_vector[3] u;
  sum_to_zero_vector[4] z;
  corr_matrix[3] R;
  cov_matrix[3] S;
  cholesky_factor_cov[3] Lc;
  cholesky_factor_cov[4, 3] Lr;
}
model {
  target += a + sum(b) + c + d;
  target += sum(mu_p) + sum(sg_p) + sum(e) + 2 * e[1];
  target += sum(u) + 2 * u[1];
  target += sum(z) + 3 * z[2];
  target += sum(R) + 2 * R[1, 2];
  target += sum(S) + 2 * S[2, 3];
  target += sum(Lc) + 2 * Lc[2, 1];
  target += sum(Lr) + 2 * Lr[4, 2];
}
