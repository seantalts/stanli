// Overloaded user-defined functions (issue #125): stanc3 keeps every
// overload under the same fdname, so calls must resolve by argument type.
// The unused-looking cox_lcdf matters: its body calls the scalar overload,
// which is what exposed the last-def-wins collision.
functions {
  real cox_lccdf(real y, real mu, real bhaz, real cbhaz) {
    return -cbhaz * mu;
  }
  real cox_lccdf(vector y, vector mu, vector bhaz, vector cbhaz) {
    return -dot_product(cbhaz, mu);
  }
  real cox_lcdf(real y, real mu, real bhaz, real cbhaz) {
    return log1m_exp(cox_lccdf(y | mu, bhaz, cbhaz));
  }
}
data {
  int<lower=1> N;
  vector[N] Y;
  vector[N] bhaz;
  vector[N] cbhaz;
}
parameters {
  real alpha;
}
model {
  target += cox_lccdf(Y | alpha * bhaz, bhaz, cbhaz);
  target += cox_lcdf(Y[1] | alpha, bhaz[1], cbhaz[1]);
  target += normal_lpdf(alpha | 0, 1);
}
