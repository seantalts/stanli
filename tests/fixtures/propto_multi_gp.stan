// Each data/parameter split of these densities under ~. Stan Math drops
// the terms whose arguments are all data, so stanli has to as well.
data {
  matrix[3,4] y_multi_gp;
  cov_matrix[4] S_multi_gp;
  vector[3] w_multi_gp;
  matrix[3,4] y_multi_gp_cholesky;
  cholesky_factor_cov[4] S_multi_gp_cholesky;
  vector[3] w_multi_gp_cholesky;
}
parameters {
  matrix[3,4] y_pdd_multi_gp;
  cov_matrix[4] S_dpd_multi_gp;
  vector<lower=0>[3] w_ddp_multi_gp;
  matrix[3,4] y_ppd_multi_gp;
  cov_matrix[4] S_ppd_multi_gp;
  matrix[3,4] y_pdp_multi_gp;
  vector<lower=0>[3] w_pdp_multi_gp;
  cov_matrix[4] S_dpp_multi_gp;
  vector<lower=0>[3] w_dpp_multi_gp;
  matrix[3,4] y_ppp_multi_gp;
  cov_matrix[4] S_ppp_multi_gp;
  vector<lower=0>[3] w_ppp_multi_gp;
  matrix[3,4] y_pdd_multi_gp_cholesky;
  cholesky_factor_cov[4] S_dpd_multi_gp_cholesky;
  vector<lower=0>[3] w_ddp_multi_gp_cholesky;
  matrix[3,4] y_ppd_multi_gp_cholesky;
  cholesky_factor_cov[4] S_ppd_multi_gp_cholesky;
  matrix[3,4] y_pdp_multi_gp_cholesky;
  vector<lower=0>[3] w_pdp_multi_gp_cholesky;
  cholesky_factor_cov[4] S_dpp_multi_gp_cholesky;
  vector<lower=0>[3] w_dpp_multi_gp_cholesky;
  matrix[3,4] y_ppp_multi_gp_cholesky;
  cholesky_factor_cov[4] S_ppp_multi_gp_cholesky;
  vector<lower=0>[3] w_ppp_multi_gp_cholesky;
}
model {
  // multi_gp
  y_multi_gp ~ multi_gp(S_multi_gp, w_multi_gp);
  y_pdd_multi_gp ~ multi_gp(S_multi_gp, w_multi_gp);
  y_multi_gp ~ multi_gp(S_dpd_multi_gp, w_multi_gp);
  y_multi_gp ~ multi_gp(S_multi_gp, w_ddp_multi_gp);
  y_ppd_multi_gp ~ multi_gp(S_ppd_multi_gp, w_multi_gp);
  y_pdp_multi_gp ~ multi_gp(S_multi_gp, w_pdp_multi_gp);
  y_multi_gp ~ multi_gp(S_dpp_multi_gp, w_dpp_multi_gp);
  y_ppp_multi_gp ~ multi_gp(S_ppp_multi_gp, w_ppp_multi_gp);
  // multi_gp_cholesky
  y_multi_gp_cholesky ~ multi_gp_cholesky(S_multi_gp_cholesky, w_multi_gp_cholesky);
  y_pdd_multi_gp_cholesky ~ multi_gp_cholesky(S_multi_gp_cholesky, w_multi_gp_cholesky);
  y_multi_gp_cholesky ~ multi_gp_cholesky(S_dpd_multi_gp_cholesky, w_multi_gp_cholesky);
  y_multi_gp_cholesky ~ multi_gp_cholesky(S_multi_gp_cholesky, w_ddp_multi_gp_cholesky);
  y_ppd_multi_gp_cholesky ~ multi_gp_cholesky(S_ppd_multi_gp_cholesky, w_multi_gp_cholesky);
  y_pdp_multi_gp_cholesky ~ multi_gp_cholesky(S_multi_gp_cholesky, w_pdp_multi_gp_cholesky);
  y_multi_gp_cholesky ~ multi_gp_cholesky(S_dpp_multi_gp_cholesky, w_dpp_multi_gp_cholesky);
  y_ppp_multi_gp_cholesky ~ multi_gp_cholesky(S_ppp_multi_gp_cholesky, w_ppp_multi_gp_cholesky);
}
