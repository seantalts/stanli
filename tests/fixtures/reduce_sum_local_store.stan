// A partial sum that fills a local container element by element, as
// occupancy models do with their per-site log likelihoods.
functions {
  real partial(array[] int sites, int first, int last, array[,] int y,
               matrix logit_p, vector logit_psi) {
    int n = last - first + 1;
    array[n] real lp;
    for (i in 1:n) {
      int site = sites[i];
      lp[i] = bernoulli_logit_lpmf(y[site] | logit_p[site]');
      if (sum(y[site]) == 0)
        lp[i] = log_sum_exp(lp[i] + log_inv_logit(logit_psi[site]),
                            log1m_inv_logit(logit_psi[site]));
      else
        lp[i] += log_inv_logit(logit_psi[site]);
    }
    return sum(lp);
  }
}
data {
  int<lower=1> I, J, grainsize;
  array[I, J] int<lower=0, upper=1> y;
}
transformed data {
  array[I] int sites = linspaced_int_array(I, 1, I);
}
parameters {
  matrix[I, J] logit_p;
  vector[I] logit_psi;
}
model {
  to_vector(logit_p) ~ normal(0, 1);
  logit_psi ~ normal(0, 1);
  target += reduce_sum(partial, sites, grainsize, y, logit_p, logit_psi);
}
