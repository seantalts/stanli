/*
    multinma is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    multinma is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with multinma.  If not, see <http://www.gnu.org/licenses/>.
*/
functions {
// Function for selecting given prior distribution, provides sampling statement ~prior_select(...)
void prior_select_lp(vector y, int dist, real location, real scale, real df) {
  if (dist == 0) { // Implicit flat prior

  } else if (dist == 1) { // Normal
    y ~ normal(location, scale);
  } else if (dist == 2) { // Cauchy
    y ~ cauchy(location, scale);
  } else if (dist == 3) { // Student t
    y ~ student_t(df, location, scale);
  } else if (dist == 4) { // Exponential
    y ~ exponential(1/scale);
  } else if (dist == 5) { // log-Normal
    y ~ lognormal(location, scale);
  } else if (dist == 6) { // log-Student t
    log(y) ~ student_t(df, location, scale);
    target += -log(y);
  } else {
    reject("Not a supported prior dist.");
  }
  return;
}

void prior_select2_lp(real y, int dist, real location, real scale, real df) {
  if (dist == 0) { // Implicit flat prior

  } else if (dist == 1) { // Normal
    y ~ normal(location, scale);
  } else if (dist == 2) { // Cauchy
    y ~ cauchy(location, scale);
  } else if (dist == 3) { // Student t
    y ~ student_t(df, location, scale);
  } else if (dist == 4) { // Exponential
    y ~ exponential(1/scale);
  } else if (dist == 5) { // log-Normal
    y ~ lognormal(location, scale);
  } else if (dist == 6) { // log-Student t
    log(y) ~ student_t(df, location, scale);
    target += -log(y);
  } else {
    reject("Not a supported prior dist.");
  }
  return;
}

// Count number of non-zero entries in a matrix
int count_nonzero(matrix m) {
  int I = rows(m);
  int J = cols(m);
  int c = 0;
  for (j in 1:I) {
    for (i in 1:J) {
      if (m[i,j] != 0) c += 1;
    }
  }
  return c;
}

// Which entries are greater than 0
array[] int which_gt0a(array[] int x) {
  int n = num_elements(x);
  array[n] int w; // Over-allocate w and then truncate later
  int c = 1;
  for (i in 1:n) {
    if (x[i] > 0) {
      w[c] = i;
      c += 1;
    }
  }
  return w[1:(c-1)];
}
// -- Vector functions --

// Which entries are equal to a given value
array[] int which(array[] int x, int y) {
  int n = num_elements(x);
  array[n] int w; // Over-allocate w and then truncate later
  int c = 1;
  for (i in 1:n) {
    if (x[i] == y) {
      w[c] = i;
      c += 1;
    }
  }
  return w[1:(c-1)];
}

// How many entries are equal to a given value
int nwhich(array[] int x, int y) {
  int w = 0;
  for (i in 1:num_elements(x)) {
    if (x[i] == y) w += 1;
  }
  return w;
}

array[] int nwhich_all(array[] int x, int max_id) {
  array[max_id] int w = rep_array(0, max_id);
  for (i in 1:num_elements(x)) {
    if (x[i]) w[x[i]] += 1;
  }
  return w;
}

// Which entries are greater than 0
array[] int which_gt0(vector x) {
  int n = num_elements(x);
  array[n] int w; // Over-allocate w and then truncate later
  int c = 1;
  for (i in 1:n) {
    if (x[i] > 0) {
      w[c] = i;
      c += 1;
    }
  }
  return w[1:(c-1)];
}

  // -- log-Survival and log-Hazard --
  vector lS (matrix ibasis, vector eta, matrix scoef) {
    // ibasis = integrated basis evaluated at event/censoring time
    // scoef = row_vector of spline coefficients
    // eta = log rate (linear predictor)
    return - rows_dot_product(ibasis, scoef) .* exp(eta);
  }

  vector lh (matrix basis, vector eta, matrix scoef) {
    // basis = basis evaluated at event/censoring time
    // scoef = row_vector of spline coefficients
    // eta = log rate (linear predictor)
    return log(rows_dot_product(basis, scoef)) + eta;
  }

  // Versions with a single scoef vector for all individuals
  vector lS2 (matrix ibasis, vector eta, vector scoef) {
    // ibasis = integrated basis evaluated at event/censoring time
    // scoef = row_vector of spline coefficients
    // eta = log rate (linear predictor)
    return - (ibasis * scoef) .* exp(eta);
  }

  vector lh2 (matrix basis, vector eta, vector scoef) {
    // basis = basis evaluated at event/censoring time
    // scoef = row_vector of spline coefficients
    // eta = log rate (linear predictor)
    return log(basis * scoef) + eta;
  }

  // AgD versions with integration points
  vector lS_a (row_vector ibasis, vector eta, matrix scoef) {
    // ibasis = integrated basis evaluated at event/censoring time
    // scoef = row_vector of spline coefficients
    // eta = log rate (linear predictor)
    return - (scoef * ibasis') .* exp(eta);
  }

  vector lh_a (row_vector basis, vector eta, matrix scoef) {
    // basis = basis evaluated at event/censoring time
    // scoef = row_vector of spline coefficients
    // eta = log rate (linear predictor)
    return log(scoef * basis') + eta;
  }

  vector lS_a2 (row_vector ibasis, vector eta, vector scoef) {
    // ibasis = integrated basis evaluated at event/censoring time
    // scoef = row_vector of spline coefficients
    // eta = log rate (linear predictor)
    return - (ibasis * scoef) * exp(eta);
  }

  vector lh_a2 (row_vector basis, vector eta, vector scoef) {
    // basis = basis evaluated at event/censoring time
    // scoef = row_vector of spline coefficients
    // eta = log rate (linear predictor)
    return log(basis * scoef) + eta;
  }

  // -- Log likelihood with censoring and truncation --
  vector loglik(matrix time,         // Basis evaluated at event/cens time
              matrix itime,        // Integrated basis evaluated at event/cens time
              matrix start_itime,  // Integrated basis evaluated at left interval time
              matrix delay_itime,  // Integrated basis evaluated at delayed entry time
              array[] int delayed,             // Delayed entry flag (1=delay)
              array[] int status,              // Censoring status
              vector eta,                // Linear predictor
              matrix scoef) {          // Spline coefficients

    vector[num_elements(eta)] l;
    array[3] int nw = nwhich_all(status, 3);
    int nd = nwhich(delayed, 1);

    // Right censored
    l = lS(itime, eta, scoef);

    // Observed
    if (nw[1]) {
      array[nw[1]] int w1 = which(status, 1);
      l[w1] += lh(time[w1], eta[w1], scoef[w1]);
    }

    // Left censored
    if (nw[2]) {
      array[nw[2]] int w2 = which(status, 2);
      l[w2] = log1m_exp(l[w2]);
    }

    // Interval censored
    if (nw[3]) {
      array[nw[3]] int w3 = which(status, 3);
      // l[w3] = log(exp(lS(start_itime[w3], eta[w3], scoef[w3])) - exp(l[w3]));
      l[w3] = log_diff_exp(lS(start_itime[w3], eta[w3], scoef[w3]), l[w3]);
    }

    // Left truncation
    if (nd) {
      array[nd] int wd = which(delayed, 1);
      l[wd] -= lS(delay_itime[wd], eta[wd], scoef[wd]);
    }

    return l;
  }

  // Version with single scoef vector for all individuals
  vector loglik2(matrix time,         // Basis evaluated at event/cens time
              matrix itime,        // Integrated basis evaluated at event/cens time
              matrix start_itime,  // Integrated basis evaluated at left interval time
              matrix delay_itime,  // Integrated basis evaluated at delayed entry time
              array[] int delayed,             // Delayed entry flag (1=delay)
              array[] int status,              // Censoring status
              vector eta,                // Linear predictor
              vector scoef) {          // Spline coefficients

    vector[num_elements(eta)] l;
    array[3] int nw = nwhich_all(status, 3);
    int nd = nwhich(delayed, 1);

    // Right censored
    l = lS2(itime, eta, scoef);

    // Observed
    if (nw[1]) {
      array[nw[1]] int w1 = which(status, 1);
      l[w1] += lh2(time[w1], eta[w1], scoef);
    }

    // Left censored
    if (nw[2]) {
      array[nw[2]] int w2 = which(status, 2);
      l[w2] = log1m_exp(l[w2]);
    }

    // Interval censored
    if (nw[3]) {
      array[nw[3]] int w3 = which(status, 3);
      // l[w3] = log(exp(lS2(start_itime[w3], eta[w3], scoef)) - exp(l[w3]));
      l[w3] = log_diff_exp(lS2(start_itime[w3], eta[w3], scoef), l[w3]);
    }

    // Left truncation
    if (nd) {
      array[nd] int wd = which(delayed, 1);
      l[wd] -= lS2(delay_itime[wd], eta[wd], scoef);
    }

    return l;
  }

  // AgD version with integration points over eta and scoef
  vector loglik_a(row_vector time,         // Basis evaluated at event/cens time
                  row_vector itime,        // Integrated basis evaluated at event/cens time
                  row_vector start_itime,  // Integrated basis evaluated at left interval time
                  row_vector delay_itime,  // Integrated basis evaluated at delayed entry time
                  int delayed,             // Delayed entry flag (1=delay)
                  int status,              // Censoring status
                  vector eta,              // Linear predictor
                  matrix scoef) {          // Spline coefficients
    vector[num_elements(eta)] l;

    if (status == 0) { // Right censored
      l = lS_a(itime, eta, scoef);
    } else if (status == 1) { // Observed
      l = lS_a(itime, eta, scoef) + lh_a(time, eta, scoef);
    } else if (status == 2) { // Left censored
      l = log1m_exp(lS_a(itime, eta, scoef));
    } else if (status == 3) { // Interval censored
      l = log_diff_exp(lS_a(start_itime, eta, scoef), lS_a(itime, eta, scoef));
      // l = log(exp(lS_a(start_itime, eta, scoef)) - exp(lS_a(itime, eta, scoef)));
    }

    // Left truncation
    if (delayed) {
      l -= lS_a(delay_itime, eta, scoef);
    }

    return l;
  }

  // AgD version integration points over eta only (single scoef vector)
  vector loglik_a2(row_vector time,         // Basis evaluated at event/cens time
                   row_vector itime,        // Integrated basis evaluated at event/cens time
                   row_vector start_itime,  // Integrated basis evaluated at left interval time
                   row_vector delay_itime,  // Integrated basis evaluated at delayed entry time
                   int delayed,             // Delayed entry flag (1=delay)
                   int status,              // Censoring status
                   vector eta,              // Linear predictor
                   vector scoef) {          // Spline coefficients
    vector[num_elements(eta)] l;

    if (status == 0) { // Right censored
      l = lS_a2(itime, eta, scoef);
    } else if (status == 1) { // Observed
      l = lS_a2(itime, eta, scoef) + lh_a2(time, eta, scoef);
    } else if (status == 2) { // Left censored
      l = log1m_exp(lS_a2(itime, eta, scoef));
    } else if (status == 3) { // Interval censored
      l = log_diff_exp(lS_a2(start_itime, eta, scoef), lS_a2(itime, eta, scoef));
      // l = log(exp(lS_a2(start_itime, eta, scoef)) - exp(lS_a2(itime, eta, scoef)));
    }

    // Left truncation
    if (delayed) {
      l -= lS_a2(delay_itime, eta, scoef);
    }

    return l;
  }

  // Compound symmetry correlation matrix
  matrix cs(real rho, int N) {
    matrix[N, N] out;
    for (j in 1:N) for (i in 1:N) {
      if (i == j) out[i, j] = 1;
      else out[i, j] = rho;
    }
    return out;
  }

}
data {
// Common definitions for the data block
// -- Constants --
int<lower=0> ns_ipd; // number of IPD studies
int<lower=0> ns_agd_arm; // number of AgD (arm-based) studies
int<lower=0> ns_agd_contrast; // number of AgD (contrast-based) studies

int<lower=0> ni_ipd; // total number of IPD individuals
int<lower=0> ni_agd_arm; // total number of AgD (arm-based) data points
int<lower=0> ni_agd_contrast; // total number of AgD (contrast-based) data points

// Treatment IDs
int<lower=0> narm_ipd; // Number of IPD arms
array[ni_ipd] int<lower=1> ipd_arm; // Arm indicator for IPD (i.e. picking element of which_RE)
array[narm_ipd] int<lower=1> ipd_trt;
int<lower=0> narm_agd_arm;
array[narm_agd_arm] int<lower=1> agd_arm_trt;
array[ni_agd_contrast] int<lower=1> agd_contrast_trt;
array[ni_agd_contrast] int<lower=1> agd_contrast_trt_b;

// Study IDs
// array[max(ipd_arm)] int<lower=1> ipd_study;
// array[ni_agd_arm] int<lower=1> agd_arm_study;
// array[ni_agd_contrast] int<lower=1> agd_contrast_study;

int<lower=1> nt; // number of treatments
int<lower=0> nX; // number of columns of design matrix

// Integration
int<lower=1> nchains;
int<lower=1,upper=nchains> CHAIN_ID;
array[nchains] int<lower=1> nint_vec; // number of samples for numerical integration (1 = no integration)
int<lower=1> nint_max;
int<lower=0> int_thin; // thinning factor for saved integration points

int<lower=1> link; // link function

// -- AgD (contrast-based) outcomes
vector[ni_agd_contrast] agd_contrast_y;
cov_matrix[ni_agd_contrast ? ni_agd_contrast : 1] agd_contrast_Sigma;

// -- Design matrix or thin QR decomposition --
int<lower=0, upper=1> QR; // use QR decomposition (yes = 1)
matrix[ni_ipd + nint_max * (ni_agd_arm + ni_agd_contrast), nX] X; // X is Q from QR decomposition if QR = 1
matrix[QR ? nX : 0, QR ? nX : 0] R_inv;

// -- Offsets --
int<lower=0, upper=1> has_offset; // Offset flag (yes = 1)
vector[has_offset ? ni_ipd + nint_max * (ni_agd_arm + ni_agd_contrast) : 0] offsets; // Vector of offsets

// -- Random effects --
int<lower=0, upper=1> RE; // Random effects flag (yes = 1)
array[RE ? narm_ipd + narm_agd_arm + ni_agd_contrast : 0] int<lower=0> which_RE; // ID of RE delta for each arm (0 for no RE delta)
corr_matrix[RE ? max(which_RE) : 1] RE_cor; // RE correlation matrix

// -- Node-splitting --
int<lower=0, upper=1> nodesplit; // Node-splitting flag (yes = 1)

// -- Baseline risk meta-regression --
int<lower=0> brmr_n_col;
array[brmr_n_col] int<lower=1> brmr_col;
real xbar_mu;

// -- Priors --
int<lower=0,upper=3> prior_intercept_dist;
real prior_intercept_location;
real<lower=0> prior_intercept_scale;
real<lower=0> prior_intercept_df;

int<lower=0,upper=3> prior_trt_dist;
real prior_trt_location;
real<lower=0> prior_trt_scale;
real<lower=0> prior_trt_df;

int<lower=0,upper=6> prior_het_dist;
int<lower=1,upper=3> prior_het_type;
real prior_het_location;
real<lower=0> prior_het_scale;
real<lower=0> prior_het_df;

int<lower=0,upper=3> prior_reg_dist;
real prior_reg_location;
real<lower=0> prior_reg_scale;
real<lower=0> prior_reg_df;

// -- class effects model --
int<lower=0, upper=1> class_effects; // Flag for whether an exchangeable class model is used (1) or not (0)
array[class_effects ? nt-1 : 0] int<lower=0> which_CE; // Class ID for each treatment (0 for no class effect)
array[class_effects ? nt-1 : 0] int<lower=0> which_CE_sd; // Design vector for class standard deviations

int<lower=0,upper=3> prior_class_mean_dist; // Prior specifications for class means
real prior_class_mean_location;
real<lower=0> prior_class_mean_scale;
real<lower=0> prior_class_mean_df;

int<lower=0,upper=6> prior_class_sd_dist; // Prior specifications for class standard deviations
real prior_class_sd_location;
real<lower=0> prior_class_sd_scale;
real<lower=0> prior_class_sd_df;

  // Number of spline coefficients
  int<lower=2> n_scoef;

  // AgD arm-based
  // int<lower=0> narm_agd_arm; // Number of arm-based AgD arms
  array[ni_agd_arm] int<lower=1> agd_arm_arm; // Arm indicator for AgD (arm-based) (i.e. picking element of which_RE)

  // Outcomes
  matrix[ni_ipd, n_scoef] ipd_time;
  matrix[ni_ipd, n_scoef] ipd_itime;
  matrix[ni_ipd, n_scoef] ipd_start_itime;
  matrix[ni_ipd, n_scoef] ipd_delay_itime;
  array[ni_ipd] int<lower=0, upper=1> ipd_delayed;
  array[ni_ipd] int<lower=0, upper=3> ipd_status;

  matrix[ni_agd_arm, n_scoef] agd_arm_time;
  matrix[ni_agd_arm, n_scoef] agd_arm_itime;
  matrix[ni_agd_arm, n_scoef] agd_arm_start_itime;
  matrix[ni_agd_arm, n_scoef] agd_arm_delay_itime;
  array[ni_agd_arm] int<lower=0, upper=1> agd_arm_delayed;
  array[ni_agd_arm] int<lower=0, upper=3> agd_arm_status;

  // aux IDs for independent spline coefficients parameters
  int<lower=0, upper=1> aux_int; // Flag aux regression needs integration (1 = yes)
  array[ni_ipd + ni_agd_arm*(aux_int ? nint_max : 1)] int<lower=1> aux_id;
  array[ni_ipd + ni_agd_arm*(aux_int ? nint_max : 1)] int<lower=1> aux_group;

  // auxiliary design matrix
  int<lower=0> nX_aux;
  matrix[ni_ipd + (aux_int ? nint_max : 1) * ni_agd_arm, nX_aux] X_aux;
  int<lower=0, upper=1> aux_reg_trt; // Flag aux regression includes main treatment effects (1 = yes)

  // Hierarchical logistic prior on spline coefficients
  int<lower=0,upper=6> prior_hyper_dist;
  real<lower=0> prior_hyper_location;
  real<lower=0> prior_hyper_scale;
  real<lower=0> prior_hyper_df;
  array[nX_aux ? 1 : max(aux_id)] vector[n_scoef-1] prior_aux_location;  // prior logistic mean
  array[nX_aux ? 1 : max(aux_id)] vector[n_scoef-1] lscoef_weight;  // rw1 prior weights

  // Hyperprior on aux regression smooths
  int<lower=0,upper=6> prior_reg_hyper_dist;
  real<lower=0> prior_reg_hyper_location;
  real<lower=0> prior_reg_hyper_scale;
  real<lower=0> prior_reg_hyper_df;
}
transformed data {
  // Number of aux coefficient vectors
  int n_aux = max(aux_id);
  int n_aux_group = max(aux_group);

  // Split aux_id by IPD and AgD rows
  array[ni_ipd] int<lower=1> aux_id_ipd = aux_id[1:ni_ipd];
  array[ni_agd_arm*(aux_int ? nint_max : 1)] int<lower=1> aux_id_agd_arm = aux_id[(ni_ipd + 1):(ni_ipd + ni_agd_arm*(aux_int ? nint_max : 1))];

  array[ni_ipd] int<lower=1> aux_group_ipd = aux_group[1:ni_ipd];
  array[ni_agd_arm*(aux_int ? nint_max : 1)] int<lower=1> aux_group_agd_arm = aux_group[(ni_ipd + 1):(ni_ipd + ni_agd_arm*(aux_int ? nint_max : 1))];

  // Aux ID indexing arrays
  array[aux_int == 0 ? n_aux_group : 0] int ni_aux_group_ipd = nwhich_all(aux_group_ipd, n_aux_group);
  array[aux_int == 0 ? n_aux_group : 0, max(ni_aux_group_ipd)] int wi_aux_group_ipd;
  array[aux_int == 0 ? n_aux_group : 0] int ni_aux_group_agd_arm = nwhich_all(aux_group_agd_arm, n_aux_group);
  array[aux_int == 0 ? n_aux_group : 0, max(ni_aux_group_agd_arm)] int wi_aux_group_agd_arm;

  // Split spline X matrix into IPD and AgD rows
  matrix[ni_ipd, nX_aux] X_aux_ipd;
  matrix[(aux_int ? nint_max : 1) * ni_agd_arm, nX_aux] X_aux_agd_arm;

  cholesky_factor_corr[aux_reg_trt ? nt : 0] sigma_beta_L;

// Common definitions for the transformed data block

// -- Integration --
// Set nint by chain
int<lower=1,upper=nint_max> nint = nint_vec[CHAIN_ID];

// -- Random effects --
// number of random effects
int<lower=0> n_delta = RE ? max(which_RE) : 0;
// RE MVN mean and correlations
vector[n_delta] RE_mu = rep_vector(0, n_delta);
// Cholesky decomposition of RE MVN correlations
matrix[0, 0] REdummy; // Use dummy zero-dim matrix to set RE_L to [0x0] when n_delta = 0
cholesky_factor_corr[n_delta] RE_L = n_delta ? cholesky_decompose(RE_cor) : REdummy;
// Sparse representation
vector[0] wdummy;
array[0] int vudummy;
int RE_L_nz = count_nonzero(RE_L); // Number of non-zero entries
int RE_sparse = RE_L_nz * 1.0 / num_elements(RE_L) <= 0.1; // Use sparse representation? (yes = 1)
vector[RE_sparse ? RE_L_nz : 0] RE_L_w = RE_sparse ? csr_extract_w(RE_L): wdummy; // Non-zero entries
array[RE_sparse ? RE_L_nz : 0] int RE_L_v = RE_sparse ? csr_extract_v(RE_L): vudummy; // v sparse component
array[RE_sparse ? n_delta + 1 : 0] int RE_L_u = RE_sparse ? csr_extract_u(RE_L) : vudummy; // u sparse component

// Total number of data points
// int totni = ni_ipd + nint * (ni_agd_arm + ni_agd_contrast);

// Total number of study intercepts (none for contrast-based data)
int totns = ns_ipd + ns_agd_arm; // + ns_agd_contrast;

// Number of IPD arms
// int<lower=0> narm_ipd = ni_ipd ? max(ipd_arm) : 0;

// All treatments vector
array[narm_ipd + narm_agd_arm + ni_agd_contrast] int<lower=1> trt = append_array(append_array(ipd_trt, agd_arm_trt), agd_contrast_trt);

// Split Q matrix or X matrix into IPD and AgD rows
matrix[0, nX] Xdummy;
matrix[ni_ipd, nX] X_ipd = ni_ipd ? X[1:ni_ipd] : Xdummy;
matrix[nint_max * ni_agd_arm, nX] X_agd_arm = ni_agd_arm ? X[(ni_ipd + 1):(ni_ipd + nint_max * ni_agd_arm)] : Xdummy;
matrix[nint_max * ni_agd_contrast, nX] X_agd_contrast = ni_agd_contrast ? X[(ni_ipd + nint_max * ni_agd_arm + 1):(ni_ipd + nint_max * (ni_agd_arm + ni_agd_contrast))] : Xdummy;

// Split offsets into IPD and AgD rows
vector[0] odummy;
vector[has_offset && ni_ipd ? ni_ipd : 0] offset_ipd = has_offset && ni_ipd ? offsets[1:ni_ipd] : odummy;
vector[has_offset && ni_agd_arm ? nint_max * ni_agd_arm : 0] offset_agd_arm = has_offset && ni_agd_arm ? offsets[(ni_ipd + 1):(ni_ipd + nint_max * ni_agd_arm)] : odummy;
vector[has_offset && ni_agd_contrast ? nint_max * ni_agd_contrast : 0] offset_agd_contrast = has_offset && ni_agd_contrast ? offsets[(ni_ipd + nint_max * ni_agd_arm + 1):(ni_ipd + nint_max * (ni_agd_arm + ni_agd_contrast))] : odummy;

// nint/int_thin for numerical integration checks
int n_int_thin = (nint_max > 1 && int_thin > 0) ? nint %/% int_thin : 0;

// Inverse covariance matrix for contrasts
matrix[ni_agd_contrast ? ni_agd_contrast : 1, ni_agd_contrast ? ni_agd_contrast : 1] inv_Sigma = inverse_spd(agd_contrast_Sigma);

// Construct number of contrasts in each study for contrast-based AgD by looking at Sigma covariance matrix
// NOTE: Sigma must be block diagonal (i.e. all contrasts for a single study together)
array[ns_agd_contrast] int nc_agd_contrast;
if (ns_agd_contrast) {
  int s = 1;
  int c = 1;
  for (i in 1:(ni_agd_contrast - 1)) {
    if (agd_contrast_Sigma[i, i+1] == 0) {
      nc_agd_contrast[s] = c;
      s += 1;
      c = 1;
    } else {
      c += 1;
    }
  }
  // for i = ni_agd_contrast
  nc_agd_contrast[s] = c;
}

// Pick out treatments with class effects
int<lower=0> n_class_trts = num_elements(which_gt0a(which_CE)); // Number of treatments with class effects
array[n_class_trts] int which_class_trt = which_gt0a(which_CE); // Vector mapping classes to treatments

// Pick out elements of zero-mean random class effects f_class by treatment
int<lower=0> n_class = class_effects ? max(which_CE) : 0; // Number of classes
array[class_effects ? nt-1 : 0] int<lower=0> which_fclass; // Vector to mapping treatments to f_class

if (class_effects) {
  int count = 1;  // Counter for nonzero elements in f_class

  for (t in 1:(nt - 1)) {
    if (which_CE_sd[t] > 0) {
      which_fclass[t] = count;
      count += 1;
    } else {
      which_fclass[t] = 0;  // No class effect for this treatment
    }
  }
}

  if (nX_aux) {
    if (ni_ipd) X_aux_ipd = X_aux[1:ni_ipd, ];
    if (ni_agd_arm) X_aux_agd_arm = X_aux[(ni_ipd + 1):(ni_ipd + (aux_int ? nint_max : 1) * ni_agd_arm), ];
  }

  if (aux_int == 0) for (i in 1:n_aux_group) {
    if (ni_aux_group_ipd[i]) wi_aux_group_ipd[i, 1:ni_aux_group_ipd[i]] = which(aux_group_ipd, i);
    if (ni_aux_group_agd_arm[i]) wi_aux_group_agd_arm[i, 1:ni_aux_group_agd_arm[i]] = which(aux_group_agd_arm, i);
  }

  if (aux_reg_trt) sigma_beta_L = cholesky_decompose(cs(0.5, nt));
}
parameters {
// Common definitions for the parameters block
// Parameters (on QR scale if QR = 1)
vector[nX] beta_tilde;

// (Uncorrelated) random effects and heterogeneity SD
vector[n_delta] u_delta;
vector<lower = 0>[RE ? 1 : 0] tau;

// New vector parameters for class effects model
vector[class_effects ? max(which_CE) : 0] class_mean; // Class means (zero-length if no exchangeable class effects)
vector<lower=0>[class_effects ? max(which_CE_sd) : 0] class_sd; // Class standard deviations (zero-length if no exchangeable class effects)

// randomised part of our non-centred parameterisation
vector[class_effects ? n_class_trts : 0] z_class;

  // Spline coefficient regression with shrinkage over time
  matrix[nX_aux, n_scoef-1] u_beta_aux;
  vector<lower=0>[aux_reg_trt ? nX_aux - (nt - 1) : nX_aux] sigma_beta;

  // Spline shrinkage SDs and non-centered parameterisation smoothing
  vector<lower=0>[n_aux] sigma;
  array[n_aux] vector[n_scoef-1] u_aux;
}
transformed parameters {
  // Log likelihood contributions
  vector[ni_ipd] log_L_ipd;
  vector[ni_agd_arm] log_L_agd_arm;

  // Spline coefficients
  array[n_aux] vector[n_scoef-1] lscoef; // logit baseline spline coefficients
  array[nX_aux ? 0 : n_aux] vector[n_scoef] scoef_temp;

  // Spline coefficient regression
  matrix[nX_aux, n_scoef-1] beta_aux;

// Common definitions for the transformed parameters block

// -- Linear and transformed predictors --
vector[ni_ipd] eta_ipd; // IPD linear predictor
// vector[ni_ipd] theta_ipd; // IPD transformed predictor

// -- RE deltas --
// Avoid evaluating tau[1] when no RE (u_delta is zero dim in this case)
vector[n_delta] f_delta =
  RE ? (
    RE_sparse ?
      tau[1] * csr_matrix_times_vector(n_delta, n_delta, RE_L_w, RE_L_v, RE_L_u, u_delta) :
      tau[1] * RE_L * u_delta
  ) : u_delta;

// -- Class effects --
vector[class_effects ? max(which_fclass) : 0] f_class;

// -- Back-transformed parameters --
vector[nX] allbeta = QR ? R_inv * beta_tilde : beta_tilde;
// Study baselines
vector[totns] mu;
// Treatment effects
vector[nt - 1] d = allbeta[(totns +1):(totns + nt - 1)];
// Node-splitting omega ()
vector[nodesplit] omega; // nodesplit ? allbeta[totns + ns] : vector(0);
// Regression predictors
vector[nX - totns - (nt - 1) - nodesplit] beta;

// -- AgD integration --
// vector[nint_max > 1 ? nint * ni_agd_arm : 0] theta_agd_arm_ii;
// vector[ni_agd_arm] theta_agd_arm_bar;
vector[nint_max > 1 ? nint * ni_agd_contrast : 0] eta_agd_contrast_ii;
vector[ni_agd_contrast] eta_agd_contrast_bar;


// -- Study baselines --
// Pull out mu from allbeta
if (totns) {
  mu = allbeta[1:totns];
}

// -- Regression predictors --
// Pull out beta from allbeta
if (nX - totns - (nt - 1) - nodesplit) {
  beta = allbeta[(totns + nt + nodesplit):];
}

// -- Node-splitting --
// Pull out omega from allbeta
if (nodesplit) {
  omega[1] = allbeta[totns + nt];
}

// -- Class effects --
if (class_effects) {
  f_class = class_mean[which_CE[which_class_trt]] - d[which_class_trt] + class_sd[which_CE_sd[which_class_trt]] .* z_class;
  d[which_class_trt] = class_mean[which_CE[which_class_trt]] + class_sd[which_CE_sd[which_class_trt]] .* z_class;
}

// -- IPD model --
// We define the IPD and AgD models here in the transformed parameters block,
// as the linear predictors are required to calculate the log likelihood
// later on. This is slightly more inefficient than defining the models
// locally in the model block.
if (ni_ipd) {
  if (RE) {
    {
      vector[ni_ipd] eta_ipd_noRE = has_offset ?
      X_ipd * beta_tilde + offset_ipd :
      X_ipd * beta_tilde;

      for (i in 1:ni_ipd) {
        if (which_RE[ipd_arm[i]])
        eta_ipd[i] = eta_ipd_noRE[i] + f_delta[which_RE[ipd_arm[i]]];
        else
        eta_ipd[i] = eta_ipd_noRE[i];
      }
    }
  } else {
    eta_ipd = has_offset ?
    X_ipd * beta_tilde + offset_ipd :
    X_ipd * beta_tilde;
  }

  // Baseline risk meta-regression
  if (brmr_n_col > 0) {
    // Subtracting 1 from the centred baseline risk here, as the associated
    // beta was already added once to the linear predictor by
    // `X_ipd * beta_tilde`
    eta_ipd += (X_ipd[,1:totns] * mu - xbar_mu - 1) .* (X_ipd[,brmr_col] * beta_tilde[brmr_col]);
  }

  // Add class effects contribution
  if (class_effects) {
    for (i in 1:ni_ipd) {
      if (ipd_trt[ipd_arm[i]] > 1 && which_CE[ipd_trt[ipd_arm[i]] - 1]) {
        eta_ipd[i] += f_class[which_fclass[ipd_trt[ipd_arm[i]] - 1]];
      }
    }
  }
}

// -- AgD model (contrast-based) --
if (ni_agd_contrast) {
  if (nint_max > 1) {
    vector[nint_max * ni_agd_contrast] eta_agd_contrast_noRE = has_offset ?
    X_agd_contrast * beta_tilde + offset_agd_contrast :
    X_agd_contrast * beta_tilde;

    // Add class effects contribution
    if (class_effects) {
      for (i in 1:ni_agd_arm){
        if (agd_contrast_trt[i] > 1 && which_CE[agd_contrast_trt[i] - 1]) {
          eta_agd_contrast_noRE[(1 + (i-1)*nint_max):((i-1)*nint_max + nint)] += f_class[which_fclass[agd_contrast_trt[i] - 1]];
        }
        if (agd_contrast_trt_b[i] > 1 && which_CE[agd_contrast_trt_b[i] - 1]) {
          eta_agd_contrast_noRE[(1 + (i-1)*nint_max):((i-1)*nint_max + nint)] -= f_class[which_fclass[agd_contrast_trt_b[i] - 1]];
        }
      }
    }

    if (RE) {
      for (i in 1:ni_agd_contrast) {
        if (which_RE[narm_ipd + narm_agd_arm + i])
        eta_agd_contrast_ii[(1 + (i-1)*nint):(i*nint)] =
        eta_agd_contrast_noRE[(1 + (i-1)*nint_max):((i-1)*nint_max + nint)] + f_delta[which_RE[narm_ipd + narm_agd_arm + i]];
        else
        eta_agd_contrast_ii[(1 + (i-1)*nint):(i*nint)] =
        eta_agd_contrast_noRE[(1 + (i-1)*nint_max):((i-1)*nint_max + nint)];

        eta_agd_contrast_bar[i] = mean(eta_agd_contrast_ii[(1 + (i-1)*nint):(i*nint)]);
      }
    } else {
      if (nint == nint_max) eta_agd_contrast_ii = eta_agd_contrast_noRE;
      else for (i in 1:ni_agd_contrast) {
        eta_agd_contrast_ii[(1 + (i-1)*nint):(i*nint)] = eta_agd_contrast_noRE[(1 + (i-1)*nint_max):((i-1)*nint_max + nint)];
      }

      for (i in 1:ni_agd_contrast) {
        eta_agd_contrast_bar[i] = mean(eta_agd_contrast_ii[(1 + (i-1)*nint):(i*nint)]);
      }
    }
  } else {
    if (RE) {
      vector[nint * ni_agd_contrast] eta_agd_contrast_noRE = has_offset ?
      X_agd_contrast * beta_tilde + offset_agd_contrast :
      X_agd_contrast * beta_tilde;

      for (i in 1:ni_agd_contrast) {
        if (which_RE[narm_ipd + narm_agd_arm + i])
        eta_agd_contrast_bar[i] = eta_agd_contrast_noRE[i] + f_delta[which_RE[narm_ipd + narm_agd_arm + i]];
        else
        eta_agd_contrast_bar[i] = eta_agd_contrast_noRE[i];
      }
    } else {
      eta_agd_contrast_bar = has_offset ?
      X_agd_contrast * beta_tilde + offset_agd_contrast :
      X_agd_contrast * beta_tilde;
    }

    if (class_effects) {
      for (i in 1:ni_agd_contrast) {
        if (agd_contrast_trt[i] > 1 && which_CE[agd_contrast_trt[i] - 1]) {
          eta_agd_contrast_bar[i] += f_class[which_fclass[agd_contrast_trt[i] - 1]];
        }
        if (agd_contrast_trt_b[i] > 1 && which_CE[agd_contrast_trt_b[i] - 1]) {
          eta_agd_contrast_bar[i] -= f_class[which_fclass[agd_contrast_trt_b[i] - 1]];
        }
      }
    }
  }
}

  if (nX_aux) {
    // Compound symetric random walk prior on aux regression treatment effects
    if (aux_reg_trt) {
      matrix[nt, n_scoef-1] ucorr_beta_aux = sigma_beta_L * u_beta_aux[1:nt, ];
      for (i in 1:nt) {
        beta_aux[i, ] = cumulative_sum(ucorr_beta_aux[i, ] .* to_row_vector(lscoef_weight[1])) * sigma_beta[1];
      }
      // Random walk prior on remaining aux regression pars
      for (i in (nt + 1):nX_aux) {
        beta_aux[i, ] = cumulative_sum(u_beta_aux[i, ] .* to_row_vector(lscoef_weight[1])) * sigma_beta[1 + i - nt];
      }
    } else {
      // Random walk prior on aux regression pars
      for (i in 1:nX_aux) {
        beta_aux[i, ] = cumulative_sum(u_beta_aux[i, ] .* to_row_vector(lscoef_weight[1])) * sigma_beta[i];
      }
    }
  }

  // Construct spline coefficients with random walk prior around constant hazard
  if (nX_aux) {
    for (i in 1:n_aux) {
      lscoef[i] = cumulative_sum(u_aux[i] .* lscoef_weight[1]) * sigma[i] + prior_aux_location[1];
    }
  } else {
    for (i in 1:n_aux) {
      lscoef[i] = cumulative_sum(u_aux[i] .* lscoef_weight[i]) * sigma[i] + prior_aux_location[i];
      scoef_temp[i] = softmax(append_row(0, lscoef[i]));
    }
  }


  // Evaluate log likelihood
  if (ni_ipd) {

    if (aux_int) {
      matrix[ni_ipd, n_scoef] scoef_ipd;
      matrix[ni_ipd, n_scoef-1] Xb_aux = X_aux_ipd * beta_aux;
      for (i in 1:ni_ipd) {
        scoef_ipd[i, ] = to_row_vector(softmax(append_row(0, lscoef[aux_id_ipd[i]] + to_vector(Xb_aux[i, ]))));
      }

      log_L_ipd = loglik(ipd_time,
                         ipd_itime,
                         ipd_start_itime,
                         ipd_delay_itime,
                         ipd_delayed,
                         ipd_status,
                         eta_ipd,
                         scoef_ipd);
    } else {
      // Loop over aux parameters (i.e. by study, possibly further stratified by aux_by)
      for (i in 1:n_aux_group) {
        int ni = ni_aux_group_ipd[i];

        if (ni) {
          array[ni] int wi = wi_aux_group_ipd[i, 1:ni];

          if (nX_aux) {
            row_vector[n_scoef-1] Xb_auxi = X_aux_ipd[wi[1], ] * beta_aux;
            log_L_ipd[wi] = loglik2(ipd_time[wi],
                               ipd_itime[wi],
                               ipd_start_itime[wi],
                               ipd_delay_itime[wi],
                               ipd_delayed[wi],
                               ipd_status[wi],
                               eta_ipd[wi],
                               softmax(append_row(0, lscoef[aux_id_ipd[wi[1]]] + to_vector(Xb_auxi))));
          } else {
            log_L_ipd[wi] = loglik2(ipd_time[wi],
                               ipd_itime[wi],
                               ipd_start_itime[wi],
                               ipd_delay_itime[wi],
                               ipd_delayed[wi],
                               ipd_status[wi],
                               eta_ipd[wi],
                               scoef_temp[i]);  // If no X_aux, aux_id = aux_group
          }
        }
      }
    }
  }

  // -- AgD model (arm-based) --
  if (ni_agd_arm) {
    vector[nint_max * ni_agd_arm] eta_agd_arm_noRE = has_offset ?
              X_agd_arm * beta_tilde + offset_agd_arm :
              X_agd_arm * beta_tilde;

    // Baseline risk meta-regression
    if (brmr_n_col > 0) {
      // Subtracting 1 from the centred baseline risk here, as the associated
      // beta was already added once to the linear predictor by
      // `X_agd_arm * beta_tilde`
      eta_agd_arm_noRE += (X_agd_arm[,1:totns] * mu - xbar_mu - 1) .* (X_agd_arm[,brmr_col] * beta_tilde[brmr_col]);
    }

    if (class_effects) {
      for (i in 1:ni_agd_arm) {
        if (agd_arm_trt[i] > 1 && which_CE[agd_arm_trt[i] - 1]) {
          eta_agd_arm_noRE[(1 + (i-1)*nint_max):((i-1)*nint_max + nint)] += f_class[which_fclass[agd_arm_trt[i] - 1]];
        }
      }
    }


    if (nint_max > 1) { // -- If integration points are used --

      if (aux_int) {
        for (i in 1:ni_agd_arm) {

          vector[nint] eta_agd_arm_ii;
          vector[nint] log_L_ii;
          matrix[nint, n_scoef-1] Xb_aux = X_aux_agd_arm[(1 + (i-1)*nint_max):((i-1)*nint_max + nint), ] * beta_aux;
          matrix[nint, n_scoef] scoef_agd_arm;

          eta_agd_arm_ii = eta_agd_arm_noRE[(1 + (i-1)*nint_max):((i-1)*nint_max + nint)];

          // Random effects
          if (RE && which_RE[narm_ipd + agd_arm_arm[i]]) eta_agd_arm_ii += f_delta[which_RE[narm_ipd + agd_arm_arm[i]]];

          // Average likelihood over integration points
          for (j in 1:nint) {
            scoef_agd_arm[j, ] = to_row_vector(softmax(append_row(0, lscoef[aux_id_agd_arm[(i-1)*nint_max + j]] + to_vector(Xb_aux[j, ]))));
          }
          log_L_ii = loglik_a(agd_arm_time[i],
                               agd_arm_itime[i],
                               agd_arm_start_itime[i],
                               agd_arm_delay_itime[i],
                               agd_arm_delayed[i],
                               agd_arm_status[i],
                               eta_agd_arm_ii,
                               scoef_agd_arm);

          log_L_agd_arm[i] = log_sum_exp(log_L_ii) - log(nint);

        }
      } else {
        // Loop over aux parameters first (i.e. by study, possibly further stratified by aux_by) for efficiency
        for (i in 1:n_aux_group) {
          int ni = ni_aux_group_agd_arm[i];

          if (ni) {
            array[ni] int wi = wi_aux_group_agd_arm[i, 1:ni];
            vector[n_scoef] scoef_agd_arm;

            if (nX_aux) {
              row_vector[n_scoef-1] Xb_auxi = X_aux_agd_arm[wi[1], ] * beta_aux;
              scoef_agd_arm = softmax(append_row(0, lscoef[aux_id_agd_arm[wi[1]]] + to_vector(Xb_auxi)));
            } else {
              scoef_agd_arm = scoef_temp[i];
            }

            for (j in 1:ni) {
              vector[nint] eta_agd_arm_ii;
              vector[nint] log_L_ii;

              eta_agd_arm_ii = eta_agd_arm_noRE[(1 + (wi[j]-1)*nint_max):((wi[j]-1)*nint_max + nint)];
              if (RE && which_RE[narm_ipd + agd_arm_arm[wi[j]]]) eta_agd_arm_ii += f_delta[which_RE[narm_ipd + agd_arm_arm[wi[j]]]];

              log_L_ii = loglik_a2(agd_arm_time[wi[j]],
                                   agd_arm_itime[wi[j]],
                                   agd_arm_start_itime[wi[j]],
                                   agd_arm_delay_itime[wi[j]],
                                   agd_arm_delayed[wi[j]],
                                   agd_arm_status[wi[j]],
                                   eta_agd_arm_ii,
                                   scoef_agd_arm);

              log_L_agd_arm[wi[j]] = log_sum_exp(log_L_ii) - log(nint);
            }
          }
        }
      }

    } else { // -- If no integration --
      vector[ni_agd_arm] eta_agd_arm = eta_agd_arm_noRE;

      // Random effects
      if (RE) for (i in 1:ni_agd_arm) {
        if (which_RE[narm_ipd + agd_arm_arm[i]]) eta_agd_arm[i] += f_delta[which_RE[narm_ipd + agd_arm_arm[i]]];
      }

      if (aux_int) {
        matrix[ni_agd_arm, n_scoef] scoef_agd_arm;
        matrix[ni_agd_arm, n_scoef-1] Xb_aux = X_aux_agd_arm * beta_aux;
        for (i in 1:ni_agd_arm) {
          scoef_agd_arm[i, ] = to_row_vector(softmax(append_row(0, lscoef[aux_id_agd_arm[i]] + to_vector(Xb_aux[i, ]))));
        }

        log_L_agd_arm = loglik(agd_arm_time,
                               agd_arm_itime,
                               agd_arm_start_itime,
                               agd_arm_delay_itime,
                               agd_arm_delayed,
                               agd_arm_status,
                               eta_agd_arm,
                               scoef_agd_arm);
      } else {
        // Loop over aux parameters (i.e. by study, possibly further stratified by aux_by)
        for (i in 1:n_aux_group) {
          int ni = ni_aux_group_agd_arm[i];

          if (ni) {
            array[ni] int wi = wi_aux_group_agd_arm[i, 1:ni];

            if (nX_aux) {
              row_vector[n_scoef-1] Xb_auxi = X_aux_agd_arm[wi[1], ] * beta_aux;
              log_L_agd_arm[wi] = loglik2(agd_arm_time[wi],
                                          agd_arm_itime[wi],
                                          agd_arm_start_itime[wi],
                                          agd_arm_delay_itime[wi],
                                          agd_arm_delayed[wi],
                                          agd_arm_status[wi],
                                          eta_agd_arm[wi],
                                          softmax(append_row(0, lscoef[aux_id_agd_arm[wi[1]]] + to_vector(Xb_auxi))));
            } else {
              log_L_agd_arm[wi] = loglik2(agd_arm_time[wi],
                                          agd_arm_itime[wi],
                                          agd_arm_start_itime[wi],
                                          agd_arm_delay_itime[wi],
                                          agd_arm_delayed[wi],
                                          agd_arm_status[wi],
                                          eta_agd_arm[wi],
                                          scoef_temp[i]);  // If no X_aux, aux_id = aux_group
            }
          }
        }
      }
    }
  }
}
model {
// Common definitions for the model block

// -- Priors --
// Study-specific baselines
prior_select_lp(mu, prior_intercept_dist, prior_intercept_location, prior_intercept_scale, prior_intercept_df);
// Treatment effects
  if (class_effects == 0) {
    prior_select_lp(d, prior_trt_dist, prior_trt_location, prior_trt_scale, prior_trt_df);
  } else {
    // Priors for class mean parameters
    prior_select_lp(class_mean, prior_class_mean_dist, prior_class_mean_location, prior_class_mean_scale, prior_class_mean_df);

    // Priors for class standard deviation parameters
    prior_select_lp(class_sd, prior_class_sd_dist, prior_class_sd_location, prior_class_sd_scale, prior_class_sd_df);
}

// Regression parameters
prior_select_lp(beta, prior_reg_dist, prior_reg_location, prior_reg_scale, prior_reg_df);

// Node-splitting - just use prior for d
prior_select_lp(omega, prior_trt_dist, prior_trt_location, prior_trt_scale, prior_trt_df);

// Heterogeneity
if (RE) {
  if (prior_het_type == 1) { // Prior on sd = tau
    prior_select_lp(tau, prior_het_dist, prior_het_location, prior_het_scale, prior_het_df);

  } else {{ // Prior on transformed parameter, with Jacobian adjustment
    // Transform of tau (e.g. to var or prec)
    vector[RE ? 1 : 0] tau_t;

    if (prior_het_type == 2) { // Prior on var = tau^2
      tau_t[1] = tau[1]^2;
      target += log(tau);
    }
    else if (prior_het_type == 3) { // Prior on prec = tau^-2
      tau_t[1] = tau[1]^-2;
      target += -3*log(tau);
    }
    prior_select_lp(tau_t, prior_het_dist, prior_het_location, prior_het_scale, prior_het_df);
  }}
}

// -- Random effects --
u_delta ~ std_normal();

// -- AgD model (contrast-based) --
if (ni_agd_contrast) {
  agd_contrast_y ~ multi_normal(eta_agd_contrast_bar, agd_contrast_Sigma);
}

// Class effects model for the d's
if (class_effects) {
  for (t in 1:(nt-1)) {
    if (which_CE[t] == 0) {
      // Priors for treatments without a class effect or in a single-occupancy class
      prior_select2_lp(d[t], prior_trt_dist, prior_trt_location, prior_trt_scale, prior_trt_df);
    } else {
      // Add dummy information to unused allbeta parameters to avoid Stan warnings
      allbeta[(totns + t)] ~ std_normal();
    }
  }
}

// Draw auxiliary variables from standard normal
z_class ~ std_normal();

  // -- RW1 prior on spline coefficients --
  for (i in 1:n_aux) u_aux[i] ~ std_normal(); //logistic(0, 1);
  prior_select_lp(sigma, prior_hyper_dist, prior_hyper_location, prior_hyper_scale, prior_hyper_df);

  // -- Smoothing prior on aux regression beta --
  for (i in 1:nX_aux) u_beta_aux[i] ~ std_normal();
  prior_select_lp(sigma_beta, prior_reg_hyper_dist, prior_reg_hyper_location, prior_reg_hyper_scale, prior_reg_hyper_df);

  // -- IPD likelihood --
  target += log_L_ipd;

  // -- AgD likelihood (arm-based) --
  target += log_L_agd_arm;

}
generated quantities {
  // Baseline spline coefficients
  array[n_aux] vector[n_scoef] scoef;

  // aux regression treatment effects
  matrix[aux_reg_trt ? nt-1 : 0, n_scoef-1] d_aux;

// Common definitions for the generated quantities block

// -- Log likelihood, residual deviance, fitted values --
vector[ni_ipd + ni_agd_arm + ns_agd_contrast] log_lik;
vector[ni_ipd + ni_agd_arm + ns_agd_contrast] resdev;
vector[ni_agd_contrast] fitted_agd_contrast;

// -- Estimate integration error --
  vector[ni_agd_contrast * n_int_thin] theta_bar_cum_agd_contrast;

// -- RE shrunken estimate delta --
// Note: These are the individual-level trial-specific treatment effects
vector[n_delta] delta;

if (RE) {
  int s = 1;
  for (i in 1:(narm_ipd + narm_agd_arm)) {
    if (which_RE[i]) {
      delta[s] = (trt[i] > 1 ? d[trt[i] - 1] : 0) + (which_RE[i] ? f_delta[which_RE[i]] : 0);
      s += 1;
    }
  }
  for (i in 1:ni_agd_contrast) {
    if (which_RE[narm_ipd + narm_agd_arm + i]) {
      delta[s] = (trt[narm_ipd + narm_agd_arm + i] > 1 ? d[trt[narm_ipd + narm_agd_arm + i] - 1] : 0) +
        (which_RE[narm_ipd + narm_agd_arm + i] ? f_delta[which_RE[narm_ipd + narm_agd_arm + i]] : 0);

      if (agd_contrast_trt_b[i] > 1)
        delta[s] -= d[agd_contrast_trt_b[i] - 1];

      s += 1;
    }
  }
}

// Integration error for AgD
for (i in 1:ni_agd_contrast) {
  for (j in 1:n_int_thin) {
    theta_bar_cum_agd_contrast[(i-1)*n_int_thin + j] = mean(eta_agd_contrast_ii[(1 + (i-1)*nint):((i-1)*nint + j*int_thin)]);
  }
}
// Fitted values
fitted_agd_contrast = eta_agd_contrast_bar;

// log likelihood, residual deviance for contrast-based AgD are *per study*
{
  int a = 0;
  int nc;
  for (s in 1:ns_agd_contrast) {
    nc = nc_agd_contrast[s];
    log_lik[ni_ipd + ni_agd_arm + s] = multi_normal_lpdf(agd_contrast_y[(a + 1):(a + nc)] |
      eta_agd_contrast_bar[(a + 1):(a + nc)], agd_contrast_Sigma[(a + 1):(a + nc), (a + 1):(a + nc)]);
    resdev[ni_ipd + ni_agd_arm + s] = quad_form(inv_Sigma[(a + 1):(a + nc), (a + 1):(a + nc)],
                                                agd_contrast_y[(a + 1):(a + nc)] - eta_agd_contrast_bar[(a + 1):(a + nc)]);
    a += nc;
  }
}


  // Baseline spline coefficients
  for (i in 1:n_aux) {
    scoef[i] = nX_aux ? softmax(append_row(0, lscoef[i])) : scoef_temp[i];
  }

  // aux regression treatment effects
  if (aux_reg_trt) for (i in 1:(nt-1)) {
    d_aux[i, ] = beta_aux[i+1, ] - beta_aux[1, ];
  }

  // Log likelihood
  if (ni_ipd) log_lik[1:ni_ipd] = log_L_ipd;
  if (ni_agd_arm) log_lik[(ni_ipd + 1):(ni_ipd + ni_agd_arm)] = log_L_agd_arm;

  // Residual deviance
  // NOTE: This is actually just the deviance, which is fine for relative fit model comparison
  if (ni_ipd + ni_agd_arm) resdev[1:(ni_ipd + ni_agd_arm)] = -2 * log_lik[1:(ni_ipd + ni_agd_arm)];

  // Fitted values not implemented

}
