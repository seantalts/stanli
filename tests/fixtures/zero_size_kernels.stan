// Zero-size arguments to densities, solvers and RNGs whose results agree
// with CmdStan. Each call either contributes 0 to the target or takes an
// empty argument list.
functions {
  vector rhs(real t, vector y, vector th) { return -y * (1 + sum(th)); }
  vector residual(real t, vector y, vector yp, vector th) {
    return yp + y * (1 + sum(th));
  }
  real integrand(real x, real xc, array[] real th, array[] real xr,
                 array[] int xi) {
    return exp(-x * x);
  }
  vector system(vector x, vector th) { return x - 2 - sum(th); }
}
data {
  vector[0] rt;
  matrix[0, 2] y_gp;
  vector[0] y_mst;
  array[0] vector[2] ys_mst;
  array[0] int no_counts;
  array[3] int zero_counts;
  array[2] real ts;
}
transformed data {
  array[0] real xr;
  array[0] int xi;
}
parameters {
  real<lower=0.5, upper=2> a;
  real<lower=0, upper=0.1> tau;
  real<lower=0.2, upper=0.8> beta;
  real delta;
  cov_matrix[2] S;
  cholesky_factor_cov[2] L;
  vector<lower=0>[0] w;
  real<lower=0> nu;
  vector[0] mu0;
  cov_matrix[0] S0;
  vector[2] mu;
  vector<lower=0>[0] alpha0;
  vector<lower=0>[3] alpha;
  vector[2] y0;
  vector[0] th;
  real<lower=0.5> b;
}
model {
  rt ~ wiener(a, tau, beta, delta);
  target += wiener_lpdf(rt | a, tau, beta, delta);
  y_gp ~ multi_gp(S, w);
  target += multi_gp_cholesky_lpdf(y_gp | L, w);
  y_mst ~ multi_student_t(nu, mu0, S0);
  target += multi_student_t_lpdf(y_mst | nu, mu0, S0);
  ys_mst ~ multi_student_t_cholesky(nu, mu, L);
  no_counts ~ dirichlet_multinomial(alpha0);
  zero_counts ~ dirichlet_multinomial(alpha);
  target += dirichlet_multinomial_lpmf(zero_counts | alpha);
  array[2] vector[2] z_rk45 = ode_rk45(rhs, y0, 0.0, ts, th);
  array[2] vector[2] z_ckrk = ode_ckrk(rhs, y0, 0.0, ts, th);
  array[2] vector[2] z_dae = dae_tol(residual, y0, -y0, 0.0, ts, 1e-8, 1e-8,
                                     1000, th);
  target += normal_lpdf(z_rk45[2] | 0, 1) + normal_lpdf(z_ckrk[2] | 0, 1)
            + normal_lpdf(z_dae[2] | 0, 1);
  {
    array[0] real no_theta;
    target += log(integrate_1d(integrand, 0.0, b, no_theta, xr, xi, 1e-8));
  }
  target += normal_lpdf(solve_newton(system, [1, 1]', th) | 0, 1);
  target += normal_lpdf(solve_powell(system, [1, 1]', th) | 0, 1);
  delta ~ normal(0, 1);
  nu ~ normal(3, 1);
  mu ~ normal(0, 1);
  alpha ~ normal(1, 1);
  y0 ~ normal(0, 1);
  b ~ normal(1, 1);
}
generated quantities {
  array[0] real normal_draws = normal_rng(mu0, 1);
  array[0] real exponential_draws = exponential_rng(exp(mu0));
  array[0] int multinomial_draws = multinomial_logit_rng(mu0, 0);
  array[3] int dirmult_draws = dirichlet_multinomial_rng(alpha + 0.1, 0);
  matrix[0, 0] wishart_draw = wishart_rng(3, S0);
  real after = normal_rng(delta, 1);
}
