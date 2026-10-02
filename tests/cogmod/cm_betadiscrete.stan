// generated with brms 2.23.0
functions {
  
// Log probability mass function for the (hurdle) Discrete Beta distribution
// (Sciandra et al., 2024, Sect. 3.1)
//   y     : observed rating, integer in {0, 1, ..., k}. y = 0 is only valid
//           when pzero > 0 (hurdle point mass below the 1..k rating scale)
//   mu    : mean of the underlying Beta distribution (0 < mu < 1); the
//           'liking' indicator on the logit scale
//   phi   : precision of the underlying Beta distribution (alpha + beta > 0);
//           the 'agreement' indicator on the log scale
//   pzero : probability of the hurdle point mass at 0 (0 <= pzero < 1)
//   k     : number of rating categories (fixed, passed in as data)
real cogmod_betadiscrete_lpmf(int y, real mu, real phi, real pzero, int k) {
  real alpha;
  real beta_par;
  real upper_lcdf;
  real lower_lcdf;

  if (y < 0 || y > k) {
    reject("cogmod_betadiscrete_lpmf: y must be an integer between 0 and k; found y = ", y);
  }

  if (y == 0) {
    return log(pzero);
  }

  alpha = mu * phi * 2;
  beta_par = (1 - mu) * phi * 2;

  // P(R = y) = F_B(y/k) - F_B((y-1)/k), computed on the log scale for
  // numerical stability via log_diff_exp(log(upper), log(lower)).
  upper_lcdf = (y == k) ? 0.0 : beta_lcdf(y * 1.0 / k | alpha, beta_par);
  lower_lcdf = (y == 1) ? negative_infinity() : beta_lcdf((y - 1) * 1.0 / k | alpha, beta_par);

  return log1m(pzero) + log_diff_exp(upper_lcdf, lower_lcdf);
}

}
data {
  int<lower=1> N;  // total number of observations
  array[N] int Y;  // response variable
  // data for custom integer vectors
  array[N] int vint1;
  int<lower=1> K;  // number of population-level effects
  matrix[N, K] X;  // population-level design matrix
  int<lower=1> Kc;  // number of population-level effects after centering
  int prior_only;  // should the likelihood be ignored?
}
transformed data {
  matrix[N, Kc] Xc;  // centered version of X without an intercept
  vector[Kc] means_X;  // column means of X before centering
  for (i in 2:K) {
    means_X[i - 1] = mean(X[, i]);
    Xc[, i - 1] = X[, i] - means_X[i - 1];
  }
}
parameters {
  vector[Kc] b;  // regression coefficients
  real Intercept;  // temporary intercept for centered predictors
  real Intercept_phi;  // temporary intercept for centered predictors
  real Intercept_pzero;  // temporary intercept for centered predictors
}
transformed parameters {
  // prior contributions to the log posterior
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0, 2.5);
  lprior += normal_lpdf(Intercept_phi | 0.7, 0.8);
  lprior += normal_lpdf(Intercept_pzero | -2.5, 1);
}
model {
  // likelihood including constants
  if (!prior_only) {
    // initialize linear predictor term
    vector[N] mu = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] phi = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] pzero = rep_vector(0.0, N);
    mu += Intercept + Xc * b;
    phi += Intercept_phi;
    pzero += Intercept_pzero;
    mu = inv_logit(mu);
    phi = exp(phi);
    pzero = inv_logit(pzero);
    for (n in 1:N) {
      target += cogmod_betadiscrete_lpmf(Y[n] | mu[n], phi[n], pzero[n], vint1[n]);
    }
  }
  // priors including constants
  target += lprior;
}
generated quantities {
  // actual population-level intercept
  real b_Intercept = Intercept - dot_product(means_X, b);
  // actual population-level intercept
  real b_phi_Intercept = Intercept_phi;
  // actual population-level intercept
  real b_pzero_Intercept = Intercept_pzero;
}

