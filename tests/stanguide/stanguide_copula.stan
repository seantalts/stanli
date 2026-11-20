data {
  int<lower=0> N;  // number of observations
  int<lower=0> D;  // number of dimensions
  array[N] vector<lower=0>[D] y;  // data
}

transformed data {
  vector[D] zeros = rep_vector(0, D);
}

parameters {
  // Parameters for exponential marginal distributions
  vector<lower=0>[D] lambda;  // rate parameters
  
  // Correlation matrix for Gaussian copula
  cholesky_factor_corr[D] L_Omega;
}

model {
  // Priors
  lambda ~ gamma(2, 1);  // prior for rate parameters
  L_Omega ~ lkj_corr_cholesky(2);
  
  // Likelihood using Gaussian copula with exponential marginals
  for (n in 1:N) {
    // Add exponential log density to target
    target += exponential_lpdf(y[n] | lambda);
    
    vector[D] z;
    for (d in 1:D) {
      // Transform to uniform using exponential CDF
      real u_d = exponential_cdf(y[n, d] | lambda[d]);
      
      // Transform to standard normal
      z[d] = inv_Phi(u_d);
    }
    // Multivariate normal log density with correlation matrix
    z ~ multi_normal_cholesky(zeros, L_Omega);
  }
}

generated quantities {
  // Optional: Recover correlation matrix from Cholesky factor
  matrix[D, D] Omega = multiply_lower_tri_self_transpose(L_Omega);
}
