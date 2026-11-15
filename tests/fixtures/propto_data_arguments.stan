// Each data/parameter split of these densities under ~. Stan Math drops
// the terms whose arguments are all data, so stanli has to as well.
data {
  cov_matrix[3] W_wishart;
  real nu_wishart;
  cov_matrix[3] S_wishart;
  cov_matrix[3] W_inv_wishart;
  real nu_inv_wishart;
  cov_matrix[3] S_inv_wishart;
  cholesky_factor_cov[3] W_wishart_cholesky;
  real nu_wishart_cholesky;
  cholesky_factor_cov[3] S_wishart_cholesky;
  cholesky_factor_cov[3] W_inv_wishart_cholesky;
  real nu_inv_wishart_cholesky;
  cholesky_factor_cov[3] S_inv_wishart_cholesky;
  cov_matrix[3] W_lkj_cov;
  vector[3] mu_lkj_cov;
  vector[3] sigma_lkj_cov;
  real eta_lkj_cov;
  corr_matrix[3] O_lkj_corr;
  real eta_lkj_corr;
  cholesky_factor_corr[3] L_lkj_corr_cholesky;
  real eta_lkj_corr_cholesky;
  array[3] int n_multinomial;
  simplex[3] theta_multinomial;
  array[3] int n_multinomial_logit;
  vector[3] g_multinomial_logit;
  array[3] int n_dirichlet_multinomial;
  vector[3] alpha_dirichlet_multinomial;
  real y_wiener;
  real alpha_wiener;
  real tau_wiener;
  real beta_wiener;
  real delta_wiener;
}
parameters {
  cov_matrix[3] W_pdd_wishart;
  real<lower=3> nu_dpd_wishart;
  cov_matrix[3] S_ddp_wishart;
  cov_matrix[3] W_ppd_wishart;
  real<lower=3> nu_ppd_wishart;
  cov_matrix[3] W_pdp_wishart;
  cov_matrix[3] S_pdp_wishart;
  real<lower=3> nu_dpp_wishart;
  cov_matrix[3] S_dpp_wishart;
  cov_matrix[3] W_ppp_wishart;
  real<lower=3> nu_ppp_wishart;
  cov_matrix[3] S_ppp_wishart;
  cov_matrix[3] W_pdd_inv_wishart;
  real<lower=3> nu_dpd_inv_wishart;
  cov_matrix[3] S_ddp_inv_wishart;
  cov_matrix[3] W_ppd_inv_wishart;
  real<lower=3> nu_ppd_inv_wishart;
  cov_matrix[3] W_pdp_inv_wishart;
  cov_matrix[3] S_pdp_inv_wishart;
  real<lower=3> nu_dpp_inv_wishart;
  cov_matrix[3] S_dpp_inv_wishart;
  cov_matrix[3] W_ppp_inv_wishart;
  real<lower=3> nu_ppp_inv_wishart;
  cov_matrix[3] S_ppp_inv_wishart;
  cholesky_factor_cov[3] W_pdd_wishart_cholesky;
  real<lower=3> nu_dpd_wishart_cholesky;
  cholesky_factor_cov[3] S_ddp_wishart_cholesky;
  cholesky_factor_cov[3] W_ppd_wishart_cholesky;
  real<lower=3> nu_ppd_wishart_cholesky;
  cholesky_factor_cov[3] W_pdp_wishart_cholesky;
  cholesky_factor_cov[3] S_pdp_wishart_cholesky;
  real<lower=3> nu_dpp_wishart_cholesky;
  cholesky_factor_cov[3] S_dpp_wishart_cholesky;
  cholesky_factor_cov[3] W_ppp_wishart_cholesky;
  real<lower=3> nu_ppp_wishart_cholesky;
  cholesky_factor_cov[3] S_ppp_wishart_cholesky;
  cholesky_factor_cov[3] W_pdd_inv_wishart_cholesky;
  real<lower=3> nu_dpd_inv_wishart_cholesky;
  cholesky_factor_cov[3] S_ddp_inv_wishart_cholesky;
  cholesky_factor_cov[3] W_ppd_inv_wishart_cholesky;
  real<lower=3> nu_ppd_inv_wishart_cholesky;
  cholesky_factor_cov[3] W_pdp_inv_wishart_cholesky;
  cholesky_factor_cov[3] S_pdp_inv_wishart_cholesky;
  real<lower=3> nu_dpp_inv_wishart_cholesky;
  cholesky_factor_cov[3] S_dpp_inv_wishart_cholesky;
  cholesky_factor_cov[3] W_ppp_inv_wishart_cholesky;
  real<lower=3> nu_ppp_inv_wishart_cholesky;
  cholesky_factor_cov[3] S_ppp_inv_wishart_cholesky;
  cov_matrix[3] W_pddd_lkj_cov;
  vector[3] mu_dpdd_lkj_cov;
  vector<lower=0>[3] sigma_ddpd_lkj_cov;
  real<lower=0> eta_dddp_lkj_cov;
  cov_matrix[3] W_ppdd_lkj_cov;
  vector[3] mu_ppdd_lkj_cov;
  cov_matrix[3] W_pdpd_lkj_cov;
  vector<lower=0>[3] sigma_pdpd_lkj_cov;
  cov_matrix[3] W_pddp_lkj_cov;
  real<lower=0> eta_pddp_lkj_cov;
  vector[3] mu_dppd_lkj_cov;
  vector<lower=0>[3] sigma_dppd_lkj_cov;
  vector[3] mu_dpdp_lkj_cov;
  real<lower=0> eta_dpdp_lkj_cov;
  vector<lower=0>[3] sigma_ddpp_lkj_cov;
  real<lower=0> eta_ddpp_lkj_cov;
  cov_matrix[3] W_pppd_lkj_cov;
  vector[3] mu_pppd_lkj_cov;
  vector<lower=0>[3] sigma_pppd_lkj_cov;
  cov_matrix[3] W_ppdp_lkj_cov;
  vector[3] mu_ppdp_lkj_cov;
  real<lower=0> eta_ppdp_lkj_cov;
  cov_matrix[3] W_pdpp_lkj_cov;
  vector<lower=0>[3] sigma_pdpp_lkj_cov;
  real<lower=0> eta_pdpp_lkj_cov;
  vector[3] mu_dppp_lkj_cov;
  vector<lower=0>[3] sigma_dppp_lkj_cov;
  real<lower=0> eta_dppp_lkj_cov;
  cov_matrix[3] W_pppp_lkj_cov;
  vector[3] mu_pppp_lkj_cov;
  vector<lower=0>[3] sigma_pppp_lkj_cov;
  real<lower=0> eta_pppp_lkj_cov;
}
model {
  // wishart
  W_wishart ~ wishart(nu_wishart, S_wishart);
  W_pdd_wishart ~ wishart(nu_wishart, S_wishart);
  W_wishart ~ wishart(nu_dpd_wishart, S_wishart);
  W_wishart ~ wishart(nu_wishart, S_ddp_wishart);
  W_ppd_wishart ~ wishart(nu_ppd_wishart, S_wishart);
  W_pdp_wishart ~ wishart(nu_wishart, S_pdp_wishart);
  W_wishart ~ wishart(nu_dpp_wishart, S_dpp_wishart);
  W_ppp_wishart ~ wishart(nu_ppp_wishart, S_ppp_wishart);
  // inv_wishart
  W_inv_wishart ~ inv_wishart(nu_inv_wishart, S_inv_wishart);
  W_pdd_inv_wishart ~ inv_wishart(nu_inv_wishart, S_inv_wishart);
  W_inv_wishart ~ inv_wishart(nu_dpd_inv_wishart, S_inv_wishart);
  W_inv_wishart ~ inv_wishart(nu_inv_wishart, S_ddp_inv_wishart);
  W_ppd_inv_wishart ~ inv_wishart(nu_ppd_inv_wishart, S_inv_wishart);
  W_pdp_inv_wishart ~ inv_wishart(nu_inv_wishart, S_pdp_inv_wishart);
  W_inv_wishart ~ inv_wishart(nu_dpp_inv_wishart, S_dpp_inv_wishart);
  W_ppp_inv_wishart ~ inv_wishart(nu_ppp_inv_wishart, S_ppp_inv_wishart);
  // wishart_cholesky
  W_wishart_cholesky ~ wishart_cholesky(nu_wishart_cholesky, S_wishart_cholesky);
  W_pdd_wishart_cholesky ~ wishart_cholesky(nu_wishart_cholesky, S_wishart_cholesky);
  W_wishart_cholesky ~ wishart_cholesky(nu_dpd_wishart_cholesky, S_wishart_cholesky);
  W_wishart_cholesky ~ wishart_cholesky(nu_wishart_cholesky, S_ddp_wishart_cholesky);
  W_ppd_wishart_cholesky ~ wishart_cholesky(nu_ppd_wishart_cholesky, S_wishart_cholesky);
  W_pdp_wishart_cholesky ~ wishart_cholesky(nu_wishart_cholesky, S_pdp_wishart_cholesky);
  W_wishart_cholesky ~ wishart_cholesky(nu_dpp_wishart_cholesky, S_dpp_wishart_cholesky);
  W_ppp_wishart_cholesky ~ wishart_cholesky(nu_ppp_wishart_cholesky, S_ppp_wishart_cholesky);
  // inv_wishart_cholesky
  W_inv_wishart_cholesky ~ inv_wishart_cholesky(nu_inv_wishart_cholesky, S_inv_wishart_cholesky);
  W_pdd_inv_wishart_cholesky ~ inv_wishart_cholesky(nu_inv_wishart_cholesky, S_inv_wishart_cholesky);
  W_inv_wishart_cholesky ~ inv_wishart_cholesky(nu_dpd_inv_wishart_cholesky, S_inv_wishart_cholesky);
  W_inv_wishart_cholesky ~ inv_wishart_cholesky(nu_inv_wishart_cholesky, S_ddp_inv_wishart_cholesky);
  W_ppd_inv_wishart_cholesky ~ inv_wishart_cholesky(nu_ppd_inv_wishart_cholesky, S_inv_wishart_cholesky);
  W_pdp_inv_wishart_cholesky ~ inv_wishart_cholesky(nu_inv_wishart_cholesky, S_pdp_inv_wishart_cholesky);
  W_inv_wishart_cholesky ~ inv_wishart_cholesky(nu_dpp_inv_wishart_cholesky, S_dpp_inv_wishart_cholesky);
  W_ppp_inv_wishart_cholesky ~ inv_wishart_cholesky(nu_ppp_inv_wishart_cholesky, S_ppp_inv_wishart_cholesky);
  // lkj_cov
  W_lkj_cov ~ lkj_cov(mu_lkj_cov, sigma_lkj_cov, eta_lkj_cov);
  W_pddd_lkj_cov ~ lkj_cov(mu_lkj_cov, sigma_lkj_cov, eta_lkj_cov);
  W_lkj_cov ~ lkj_cov(mu_dpdd_lkj_cov, sigma_lkj_cov, eta_lkj_cov);
  W_lkj_cov ~ lkj_cov(mu_lkj_cov, sigma_ddpd_lkj_cov, eta_lkj_cov);
  W_lkj_cov ~ lkj_cov(mu_lkj_cov, sigma_lkj_cov, eta_dddp_lkj_cov);
  W_ppdd_lkj_cov ~ lkj_cov(mu_ppdd_lkj_cov, sigma_lkj_cov, eta_lkj_cov);
  W_pdpd_lkj_cov ~ lkj_cov(mu_lkj_cov, sigma_pdpd_lkj_cov, eta_lkj_cov);
  W_pddp_lkj_cov ~ lkj_cov(mu_lkj_cov, sigma_lkj_cov, eta_pddp_lkj_cov);
  W_lkj_cov ~ lkj_cov(mu_dppd_lkj_cov, sigma_dppd_lkj_cov, eta_lkj_cov);
  W_lkj_cov ~ lkj_cov(mu_dpdp_lkj_cov, sigma_lkj_cov, eta_dpdp_lkj_cov);
  W_lkj_cov ~ lkj_cov(mu_lkj_cov, sigma_ddpp_lkj_cov, eta_ddpp_lkj_cov);
  W_pppd_lkj_cov ~ lkj_cov(mu_pppd_lkj_cov, sigma_pppd_lkj_cov, eta_lkj_cov);
  W_ppdp_lkj_cov ~ lkj_cov(mu_ppdp_lkj_cov, sigma_lkj_cov, eta_ppdp_lkj_cov);
  W_pdpp_lkj_cov ~ lkj_cov(mu_lkj_cov, sigma_pdpp_lkj_cov, eta_pdpp_lkj_cov);
  W_lkj_cov ~ lkj_cov(mu_dppp_lkj_cov, sigma_dppp_lkj_cov, eta_dppp_lkj_cov);
  W_pppp_lkj_cov ~ lkj_cov(mu_pppp_lkj_cov, sigma_pppp_lkj_cov, eta_pppp_lkj_cov);
  // lkj_corr
  O_lkj_corr ~ lkj_corr(eta_lkj_corr);
  // lkj_corr_cholesky
  L_lkj_corr_cholesky ~ lkj_corr_cholesky(eta_lkj_corr_cholesky);
  // multinomial
  n_multinomial ~ multinomial(theta_multinomial);
  // multinomial_logit
  n_multinomial_logit ~ multinomial_logit(g_multinomial_logit);
  // dirichlet_multinomial
  n_dirichlet_multinomial ~ dirichlet_multinomial(alpha_dirichlet_multinomial);
  // wiener
  y_wiener ~ wiener(alpha_wiener, tau_wiener, beta_wiener, delta_wiener);
}
