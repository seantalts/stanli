// generated with brms 2.23.0
functions {
  /* softplus link function inverse to 'log1p_exp'
   * Args:
   *   x: a positive scalar
   * Returns:
   *   a scalar in (-Inf, Inf)
   */
   real log_expm1(real x) {
     return log(expm1(x));
   }
  /* softplus link function inverse to 'log1p_exp' (vectorized)
   * Args:
   *   x: a positive vector
   * Returns:
   *   a vector in (-Inf, Inf)
   */
   vector log_expm1(vector x) {
     return log(expm1(x));
   }

  
// mu is the probability p of the right side (0 < mu < 1) (named 'mu' as per Stan convention for main parameters)
real cogmod_choco_lpdf(
    real y,
    real mu,          // P(right | not-mid)
    real confright,   // mu for right Beta-Gate
    real precright,   // phi for right Beta-Gate
    real confleft,    // mu_left = 1 - confleft
    real precleft,    // phi for left Beta-Gate
    real pex,         // proportion-extreme total
    real bex,         // balance of extremes (-> right vs left)
    real pmid         // P(mid)
) {
    // Hardcoded Middle-Point
    real mid = 0.5;
    real eps = 1e-10;

    // 1) Domain checks
    if (
        y < 0 || y > 1 ||
        mu < 0 || mu > 1 ||
        pmid < 0 || pmid > 1 ||
        confright <= 0 || confright >= 1 ||
        precright <= 0 ||
        confleft <= 0 || confleft >= 1 ||
        precleft <= 0 ||
        pex < 0 || pex > 1 ||
        bex < 0 || bex > 1
    ) {
        return negative_infinity();
    }

    // 2) Mixture weights
    real p_not_mid = 1 - pmid;
    real p_left = p_not_mid * (1 - mu);
    real p_right = p_not_mid * mu;

    // 3) Point mass at mid
    if (abs(y - mid) < eps) {
        return (pmid > 0) ? log(pmid) : negative_infinity();
    }

    // 4) Left segment: y in [0, mid)
    if (y < mid) {
        if (p_left <= 0) return negative_infinity();

        // Rescale to [0,1]
        real y_rescaled = y / mid;

        // Beta-Gate parameters for the left side
        real mu_left = 1 - confleft;
        real mu_ql = logit(mu_left);
        real cutzero = pex * (1 - bex);

        // point mass at 0
        if (y_rescaled < eps) {
          if (cutzero <= eps) return negative_infinity();
          return log(p_left) + log1m_inv_logit(mu_ql - logit(cutzero));
        }

        // continuous (0 < y_rescaled <= 1)
        real log_mid;
        if (cutzero <= eps) {
          log_mid = 0;  // no lower cut
        } else {
          log_mid = log_inv_logit(mu_ql - logit(cutzero));
        }
        real shape1 = mu_left * precleft * 2;
        real shape2 = (1 - mu_left) * precleft * 2;
        real log_beta = beta_lpdf(y_rescaled | shape1, shape2);
        real log_jacobian = -log(mid);
        return log(p_left) + log_mid + log_beta + log_jacobian;
    }

    // 5) Right segment: y in (mid, 1]
    if (y > mid) {
        if (p_right <= 0) return negative_infinity();

        // Rescale to [0,1]
        real y_rescaled = (y - mid) / (1 - mid);

        // Beta-Gate parameters for the right side
        real mu_right = confright;
        real mu_ql = logit(mu_right);
        real cutone = 1 - pex * bex;

        // point mass at 1
        if (1 - y_rescaled < eps) {
          if (cutone >= 1 - eps) return negative_infinity();
          return log(p_right) + log_inv_logit(mu_ql - logit(cutone));
        }

        // continuous (0 <= y_rescaled < 1)
        real log_mid;
        if (cutone >= 1 - eps) {
          log_mid = 0;  // no upper cut
        } else {
          log_mid = log1m_inv_logit(mu_ql - logit(cutone));
        }
        real shape1 = mu_right * precright * 2;
        real shape2 = (1 - mu_right) * precright * 2;
        real log_beta = beta_lpdf(y_rescaled | shape1, shape2);
        real log_jacobian = -log1m(mid);
        return log(p_right) + log_mid + log_beta + log_jacobian;
    }

    // If no branch matched, return -Inf
    return negative_infinity();
}

}
data {
  int<lower=1> N;  // total number of observations
  vector[N] Y;  // response variable
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
  real Intercept_confright;  // temporary intercept for centered predictors
  real Intercept_precright;  // temporary intercept for centered predictors
  real Intercept_confleft;  // temporary intercept for centered predictors
  real Intercept_precleft;  // temporary intercept for centered predictors
  real Intercept_pex;  // temporary intercept for centered predictors
  real Intercept_bex;  // temporary intercept for centered predictors
  real Intercept_pmid;  // temporary intercept for centered predictors
}
transformed parameters {
  // prior contributions to the log posterior
  real lprior = 0;
  lprior += student_t_lpdf(Intercept | 3, 0, 2.5);
  lprior += normal_lpdf(Intercept_confright | 0, 1);
  lprior += normal_lpdf(Intercept_precright | 2, 1.5);
  lprior += normal_lpdf(Intercept_confleft | 0, 1);
  lprior += normal_lpdf(Intercept_precleft | 2, 1.5);
  lprior += normal_lpdf(Intercept_pex | -2, 1);
  lprior += normal_lpdf(Intercept_bex | 0, 1);
  lprior += normal_lpdf(Intercept_pmid | -2.5, 1);
}
model {
  // likelihood including constants
  if (!prior_only) {
    // initialize linear predictor term
    vector[N] mu = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] confright = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] precright = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] confleft = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] precleft = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] pex = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] bex = rep_vector(0.0, N);
    // initialize linear predictor term
    vector[N] pmid = rep_vector(0.0, N);
    mu += Intercept + Xc * b;
    confright += Intercept_confright;
    precright += Intercept_precright;
    confleft += Intercept_confleft;
    precleft += Intercept_precleft;
    pex += Intercept_pex;
    bex += Intercept_bex;
    pmid += Intercept_pmid;
    mu = inv_logit(mu);
    confright = inv_logit(confright);
    precright = log1p_exp(precright);
    confleft = inv_logit(confleft);
    precleft = log1p_exp(precleft);
    pex = inv_logit(pex);
    bex = inv_logit(bex);
    pmid = inv_logit(pmid);
    for (n in 1:N) {
      target += cogmod_choco_lpdf(Y[n] | mu[n], confright[n], precright[n], confleft[n], precleft[n], pex[n], bex[n], pmid[n]);
    }
  }
  // priors including constants
  target += lprior;
}
generated quantities {
  // actual population-level intercept
  real b_Intercept = Intercept - dot_product(means_X, b);
  // actual population-level intercept
  real b_confright_Intercept = Intercept_confright;
  // actual population-level intercept
  real b_precright_Intercept = Intercept_precright;
  // actual population-level intercept
  real b_confleft_Intercept = Intercept_confleft;
  // actual population-level intercept
  real b_precleft_Intercept = Intercept_precleft;
  // actual population-level intercept
  real b_pex_Intercept = Intercept_pex;
  // actual population-level intercept
  real b_bex_Intercept = Intercept_bex;
  // actual population-level intercept
  real b_pmid_Intercept = Intercept_pmid;
}

