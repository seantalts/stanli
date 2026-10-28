//    This file is part of OncoBayes2.
//    Copyright (C) 2025 Novartis Pharma AG
/*
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
functions {

  /*
   * Author: S. Weber
   *
   * Various utlities
   */
  
  // turn a slicing variable for a ragged array
  // S = {5, 6, 11}
  // into
  // Si = {0, 5, 5 + 6, 5+6+11} + 1
  // such that we can index the ragged array A as
  // A[Si[u] : Si[u+1]-1]
  // for the uth unit
  array[] int make_slice_index(array[] int S) {
    array[size(S) + 1] int Si;
    int cv = 1;
    Si[1] = cv;
    for (i in 1 : size(S)) {
      cv = cv + S[i];
      Si[i + 1] = cv;
    }
    return Si;
  }
  
  // helper function for rle_int
  int rle_elem_count(array[] int set) {
    int U = 1;
    for (i in 2 : num_elements(set)) {
      if (set[i - 1] != set[i]) {
        U = U + 1;
      }
    }
    return U;
  }
  
  // helper function for rle_int
  int rle_elem_count_vector(vector set) {
    int U = 1;
    for (i in 2 : num_elements(set)) {
      if (set[i - 1] != set[i]) {
        U = U + 1;
      }
    }
    return U;
  }
  
  // repeated length encoding, see rle in R
  // this function returns the number of times elements are repeated ...
  array[] int rle_int(array[] int set) {
    array[rle_elem_count(set)] int res;
    int c = 1;
    res[1] = 1;
    for (i in 2 : num_elements(set)) {
      if (set[i - 1] == set[i]) {
        res[c] = res[c] + 1;
      } else {
        c = c + 1;
        res[c] = 1;
      }
    }
    return res;
  }
  
  // ... and this function returns which elements are repeated for ints
  array[] int rle_elem_int(array[] int set) {
    int N = rle_elem_count(set);
    array[N] int first_ind = make_slice_index(rle_int(set))[1 : N];
    
    return set[first_ind];
  }
  
  array[] int decimal2base(int decimal, int digits, int base) {
    array[digits] int base_rep;
    int current = decimal;
    for (i in 1 : digits) {
      base_rep[i] = current % base;
      current = current %/% base;
    }
    return base_rep;
  }
  
  int power_int(int number, int power) {
    if (power < 0) {
      reject("Cannot raise an integer to a negative power and expect an integer result.");
    }
    if (power == 0) {
      return 1;
    } else {
      return number * power_int(number, power - 1);
    }
  }
  
  // count the number of unique elements
  int cardinality_int(array[] int elems) {
    return rle_elem_count(sort_asc(elems));
  }
  
  // count the number of unique elements
  int cardinality_vector(vector elems) {
    vector[num_elements(elems)] sort_asc_elems = sort_asc(elems);
    return rle_elem_count_vector(sort_asc_elems);
  }
  
  // create an integer sequence
  array[] int seq_int(int start, int end) {
    int N = end - start + 1;
    array[N] int seq;
    for (i in 1 : N) {
      seq[i] = i + start - 1;
    }
    return seq;
  }
  
  // return an int vector with each element in the input repeated as
  // many times as given in the each argument
  array[] int rep_each(array[] int set, array[] int each) {
    int N = sum(each);
    array[N] int replicated;
    int p = 1;
    
    for (i in 1 : size(set)) {
      replicated[p : p + each[i] - 1] = rep_array(set[i], each[i]);
      p = p + each[i];
    }
    
    return replicated;
  }
  
  
  // count number times elem appears in test set
  int count_elem(array[] int test, int elem) {
    int count;
    count = 0;
    for (i in 1 : num_elements(test)) {
      if (test[i] == elem) {
        count = count + 1;
      }
    }
    return count;
  }
  
  // count number times elems appears in test set
  array[] int count_elems(array[] int test, array[] int elems) {
    array[num_elements(elems)] int counts;
    for (i in 1 : num_elements(elems)) {
      counts[i] = count_elem(test, elems[i]);
    }
    return counts;
  }
  
  // find elements in test which are equal to elem
  array[] int which_elem(array[] int test, int elem) {
    array[count_elem(test, elem)] int res;
    int ci;
    ci = 1;
    for (i in 1 : num_elements(test)) {
      if (test[i] == elem) {
        res[ci] = i;
        ci = ci + 1;
      }
    }
    return res;
  }
  
  

  int max_int(array[] int test) {
    int max_elem;
    if (size(test) == 0) reject("Test array must have length greater than 0.");
    max_elem = test[1];
    for (i in 2:size(test)) {
      max_elem = max(max_elem, test[i]);
    }
    return max_elem;
  }
  
  real mixmvnorm_lpdf(vector y, int Nc, vector w, array[] vector m, array[] matrix L) {
    vector[Nc] lp_mix;
    if(rows(y) == 0) return 0.0;
    for(i in 1:Nc) {
      lp_mix[i] = log(w[i]) + multi_normal_cholesky_lpdf(y | m[i], L[i]);
    }
    return log_sum_exp(lp_mix);
  }

  real mixmv_tau_prior_lpdf(vector tau, int dist, int Nc, vector w, array[] vector m, array[] matrix L) {
    if (dist == 0) {
      // this is the fixed density => we assign standard normals to
      // avoid sampling issues
      return std_normal_lpdf(tau);
    } else if (dist == 1) {
      vector[rows(tau)] log_tau = log(tau);
      return mixmvnorm_lpdf(log_tau | Nc, w, m, L) - sum(log_tau);
    } else if (dist == 2) {
      return mixmvnorm_lpdf(tau | Nc, w, m, L);
    }
    reject("Invalid distribution for tau.");
  }

  real tau_prior_lpdf(real tau, int dist, real a, real b) {
    if (dist == 0) {
      // this is the fixed density => we assign standard normals to
      // avoid sampling issues
      return std_normal_lpdf(tau);
    } else if (dist == 1) {
      return lognormal_lpdf(tau | a, b);
    } else if (dist == 2) {
      return normal_lpdf(tau | a, b);
    }
    reject("Invalid distribution for tau.");
  }
  
  // create for a bivariate normal it's lower triangle cholesky factor
  matrix bvn_cholesky_lower(vector tau, real rho) {
    return [[tau[1], 0.0], [tau[2] * rho, tau[2] * sqrt(1.0 - rho * rho)]];
  }
  
  /* given design matrices and parameters calculates the logit. The
   * number of tries n is only passed to sort out n=0 cases.
   */
  vector blrm_logit_fast(array[] int obs_gidx, array[] int n,
                         array[] matrix X_comp, array[,] int finite_cov,
                         matrix X_inter, array[] vector beta, vector eta) {
    int num_obs = size(obs_gidx);
    int num_comp = size(X_comp);
    int num_inter = cols(X_inter);
    vector[num_obs] mu;
    
    for (i in 1 : num_obs) {
      int idx = obs_gidx[i];
      real log_p0_nr = 0.0;
      if (n[idx] == 0) {
        // in case of no observation we merely fill in 0 as a dummy
        mu[i] = 0.0;
      } else {
        for (j in 1 : num_comp) {
          // ensure that input is finite
          if (finite_cov[j, idx]) {
            log_p0_nr += log_inv_logit(-1.0 * X_comp[j, idx] * beta[j]);
          }
        }
        // turn log(1-p0) into a logit for p0
        mu[i] = log1m_exp(log_p0_nr) - log_p0_nr;
      }
      
      // add interaction part
      if (num_inter > 0) {
        mu[i] += X_inter[idx] * eta;
      }
    }
    return mu;
  }
  
  // convenience version which creates finite_cov on the fly (for
  // prediction)
  /*
  vector blrm_logit(int[] n,
                    matrix[] X_comp,
                    matrix X_inter, 
                    vector[] beta, vector eta) {
    int num_obs = rows(X_inter);
    int num_comp = size(X_comp);
    int finite_cov[num_comp,num_obs];
  
    for(j in 1:num_comp)
      for(i in 1:num_obs) {
        finite_cov[j,i] = !is_inf(X_comp[j,i,1]) && !is_inf(X_comp[j,i,2]) ? 1 : 0;
      }
  
    return blrm_logit_fast(n, X_comp, finite_cov, X_inter, beta, eta);
  }
  */
  
  real blrm_lpmf(array[] int r, array[] int obs_gidx, array[] int n,
                 array[] matrix X_comp, array[,] int finite_cov,
                 matrix X_inter, array[] vector beta, vector eta) {
    int num_obs = size(obs_gidx);
    array[num_obs] int r_obs;
    array[num_obs] int n_obs;
    for (i in 1 : num_obs) {
      r_obs[i] = r[obs_gidx[i]];
      n_obs[i] = n[obs_gidx[i]];
    }
    return binomial_logit_lpmf(r_obs | n_obs, blrm_logit_fast(obs_gidx, n,
                                                              X_comp,
                                                              finite_cov,
                                                              X_inter, beta,
                                                              eta));
  }
  
  real blrm_lupmf_comp(array[] int r, array[] int obs_gidx, array[] int n,
                       array[] matrix X_comp, array[,] int finite_cov,
                       matrix X_inter, array[] vector beta, vector eta) {
    int num_obs = size(obs_gidx);
    array[num_obs] int r_obs;
    array[num_obs] int nr_obs;
    vector[num_obs] theta = blrm_logit_fast(obs_gidx, n, X_comp, finite_cov,
                                            X_inter, beta, eta);
    vector[num_obs] log_pi = log_inv_logit(theta);
    vector[num_obs] log_inv_pi = log_inv_logit(-1.0 * theta);
    for (i in 1 : num_obs) {
      r_obs[i] = r[obs_gidx[i]];
      nr_obs[i] = n[obs_gidx[i]] - r_obs[i];
    }
    return dot_product(to_vector(r_obs), log_pi)
           + dot_product(to_vector(nr_obs), log_inv_pi);
  }
  
  // calculates for a given group all mixture configurations and 
  vector blrm_mix_lpmf_comp(int g, int num_groups, array[] int obs_gidx,
                            array[] int r, array[] int n,
                            array[] matrix X_comp, array[,] int finite_cov,
                            matrix X_inter, array[,] vector beta,
                            array[,] int mix_idx_beta, array[] vector eta,
                            array[,] int mix_idx_eta) {
    int num_mix_comp = size(mix_idx_beta);
    int num_comp = dims(mix_idx_beta)[2];
    int num_inter = dims(mix_idx_eta)[2];
    vector[num_mix_comp] mix_lpmf;
    
    if (num_elements(r) == 0) {
      return rep_vector(0.0, num_mix_comp);
    }
    
    for (m in 1 : num_mix_comp) {
      array[num_comp] int ind_beta = mix_idx_beta[m];
      array[num_inter] int ind_eta = mix_idx_eta[m];
      array[num_comp] vector[2] beta_mix_config;
      vector[num_inter] eta_mix_config;
      for (i in 1 : num_comp) {
        beta_mix_config[i] = beta[ind_beta[i] == 1 ? g : g + num_groups, i];
      }
      for (i in 1 : num_inter) {
        eta_mix_config[i] = eta[ind_eta[i] == 1 ? g : g + num_groups, i];
      }
      
      mix_lpmf[m] = blrm_lpmf(r | obs_gidx, n, X_comp, finite_cov, X_inter, beta_mix_config, eta_mix_config);
    }
    
    return mix_lpmf;
  }
  
  // calculates for a given group all mixture configurations and
  // ... unnormalized version avoiding nasty log-gamma calls
  vector blrm_mix_lupmf_comp(int g, int num_groups, array[] int obs_gidx,
                             array[] int r, array[] int n,
                             array[] matrix X_comp, array[,] int finite_cov,
                             matrix X_inter, array[,] vector beta,
                             array[,] int mix_idx_beta, array[] vector eta,
                             array[,] int mix_idx_eta) {
    int num_mix_comp = size(mix_idx_beta);
    int num_comp = dims(mix_idx_beta)[2];
    int num_inter = dims(mix_idx_eta)[2];
    vector[num_mix_comp] mix_ll;
    
    if (num_elements(r) == 0) {
      return rep_vector(0.0, num_mix_comp);
    }
    
    for (m in 1 : num_mix_comp) {
      array[num_comp] int ind_beta = mix_idx_beta[m];
      array[num_inter] int ind_eta = mix_idx_eta[m];
      array[num_comp] vector[2] beta_mix_config;
      vector[num_inter] eta_mix_config;
      for (i in 1 : num_comp) {
        beta_mix_config[i] = beta[ind_beta[i] == 1 ? g : g + num_groups, i];
      }
      for (i in 1 : num_inter) {
        eta_mix_config[i] = eta[ind_eta[i] == 1 ? g : g + num_groups, i];
      }
      
      mix_ll[m] = blrm_lupmf_comp(r, obs_gidx, n, X_comp, finite_cov,
                                  X_inter, beta_mix_config, eta_mix_config);
    }
    
    return mix_ll;
  }

  // Performs a cholesky decompose on sub-block of the input matrix
  // whenever some columns (and respective row is 0). Zeros are
  // admitted at the beginning and the end of the matrix.
  matrix block_cholesky_decompose(matrix A) {
    int nr = rows(A);
    int nc = cols(A);
    matrix[nc, nc] L = rep_matrix(0.0, nc, nc);
    array[nr] int is_zero = rep_array(0, nr);
    int start_index = 1;
    int end_index = nr;
    if (nr != nc) reject("Can only cholesky decompose a square matrix! Columns: ", nc, "; Rows: ", nr);
    for(i in 1:nr) {
      int col_zero = 0;
      int row_zero = 0;
      for(j in 1:nr) {
        if(A[i,j] == 0) col_zero += 1;
        if(A[j,i] == 0) row_zero += 1;
      }
      if(col_zero == nc && row_zero == nr)
        is_zero[i] = 1;
    }
    if(sum(is_zero) == nr) return L;
    while(start_index != nr && is_zero[start_index] == 1) start_index += 1;
    while(start_index != end_index && is_zero[end_index] == 1) end_index -= 1;
    L[start_index:end_index, start_index:end_index] = cholesky_decompose(A[start_index:end_index, start_index:end_index]);
    return L;
  }
}
data {
  // input data is by default given in row-ordered per observation
  // format. Data should be sorted by stratum / group nesting
  int<lower=0> num_obs;
  array[num_obs] int<lower=0> r;
  array[num_obs] int<lower=0> nr;
  
  // number of components
  int<lower=1> num_comp;
  
  // for now we fix the number of regressors to two!
  array[num_comp] matrix[num_obs, 2] X_comp;
  
  // interactions
  int<lower=0> num_inter;
  matrix[num_obs, num_inter] X_inter;
  
  // data for parameter model
  
  // observation to group mapping
  array[num_obs] int<lower=1> group;
  
  // observation to stratum mapping (groups are nested into strata)
  array[num_obs] int<lower=1> stratum;
  
  // number of groups
  int<lower=1> num_groups;
  
  // number of strata
  int<lower=1> num_strata;
  
  // mapping of group_id to stratum (the index is the group id)
  array[num_groups] int<lower=1, upper=num_strata> group_stratum_cid;
  
  // definition which parameters are robust
  array[num_comp] int<lower=0, upper=1> prior_is_EXNEX_comp;
  array[num_inter] int<lower=0, upper=1> prior_is_EXNEX_inter;
  
  // per group probability for EX for each parameter which is modelled
  // as robust EX/NEX
  
  // for now enforce a minimal EX to avoid issues with 0: Todo
  // note: we define these prior probabilities even if not used
  matrix<lower=1E-6, upper=1>[num_groups, num_comp] prior_EX_prob_comp;
  matrix<lower=1E-6, upper=1>[num_groups, num_inter] prior_EX_prob_inter;
  
  // EX priors
  array[num_comp] int prior_EX_mu_comp_Nc;
  array[num_comp] vector[max_int(prior_EX_mu_comp_Nc)] prior_EX_mu_comp_w;
  array[num_comp,max_int(prior_EX_mu_comp_Nc)] vector[2] prior_EX_mu_comp_m;
  array[num_comp,max_int(prior_EX_mu_comp_Nc)] matrix[2, 2] prior_EX_mu_comp_sigma;  

  array[num_strata, num_comp] int prior_EX_tau_comp_Nc;
  array[num_strata, num_comp] vector[max_int(to_array_1d(prior_EX_tau_comp_Nc))] prior_EX_tau_comp_w;
  array[num_strata, num_comp, max_int(to_array_1d(prior_EX_tau_comp_Nc))] vector[2] prior_EX_tau_comp_m;
  array[num_strata, num_comp, max_int(to_array_1d(prior_EX_tau_comp_Nc))] matrix[2, 2] prior_EX_tau_comp_sigma;  

  array[num_comp] real<lower=0> prior_EX_corr_eta_comp;

  int prior_EX_mu_inter_Nc;
  vector[prior_EX_mu_inter_Nc] prior_EX_mu_inter_w;
  array[prior_EX_mu_inter_Nc] vector[num_inter] prior_EX_mu_inter_m;
  array[prior_EX_mu_inter_Nc] matrix[num_inter,num_inter] prior_EX_mu_inter_sigma;  

  array[num_strata] int prior_EX_tau_inter_Nc;
  array[num_strata] vector[max_int(prior_EX_tau_inter_Nc)] prior_EX_tau_inter_w;
  array[num_strata, max_int(prior_EX_tau_inter_Nc)] vector[num_inter] prior_EX_tau_inter_m;
  array[num_strata, max_int(prior_EX_tau_inter_Nc)] matrix[num_inter, num_inter] prior_EX_tau_inter_sigma;  

  
  real<lower=0> prior_EX_corr_eta_inter;
  
  // NEX priors (same for each group)
  array[num_comp] int prior_NEX_mu_comp_Nc;
  array[num_comp] vector[max_int(prior_NEX_mu_comp_Nc)] prior_NEX_mu_comp_w;
  array[num_comp,max_int(prior_NEX_mu_comp_Nc)] vector[2] prior_NEX_mu_comp_m;
  array[num_comp,max_int(prior_NEX_mu_comp_Nc)] matrix[2, 2] prior_NEX_mu_comp_sigma;  

  int prior_NEX_mu_inter_Nc;
  vector[prior_NEX_mu_inter_Nc] prior_NEX_mu_inter_w;
  array[prior_NEX_mu_inter_Nc] vector[num_inter] prior_NEX_mu_inter_m;
  array[prior_NEX_mu_inter_Nc] matrix[num_inter,num_inter] prior_NEX_mu_inter_sigma;  
 
  // prior distribution of tau's
  // 0 = fixed to its mean
  // 1 = log-normal
  // 2 = truncated normal
  int<lower=0, upper=2> prior_tau_dist;

  // controls if MAP priors for each stratum should be sampled
  int<lower=0, upper=1> sample_map;
  
  // sample from prior predictive (do not add data to likelihood)
  int<lower=0, upper=1> prior_PD;
}
transformed data {
  array[num_obs] int<lower=0> n;
  array[num_comp, num_obs] int<lower=0, upper=1> finite_cov;
  int<lower=0, upper=num_comp> num_EXNEX_comp = sum(prior_is_EXNEX_comp);
  int<lower=0, upper=num_inter> num_EXNEX_inter = sum(prior_is_EXNEX_inter);
  int<lower=0> num_mix_dim = num_EXNEX_comp + num_EXNEX_inter;
  int<lower=0> num_mix_comp = power_int(2, num_mix_dim);
  array[num_comp,max_int(prior_EX_mu_comp_Nc)] matrix[2, 2] prior_EX_mu_comp_sigma_L = rep_array(diag_matrix([1, 1]'), num_comp, max_int(prior_EX_mu_comp_Nc));
  array[num_comp,max_int(prior_NEX_mu_comp_Nc)] matrix[2, 2] prior_NEX_mu_comp_sigma_L = rep_array(diag_matrix([1, 1]'), num_comp, max_int(prior_NEX_mu_comp_Nc));
  array[prior_EX_mu_inter_Nc] matrix[num_inter, num_inter] prior_EX_mu_inter_sigma_L = rep_array(diag_matrix(rep_vector(1, num_inter)), prior_EX_mu_inter_Nc);
  array[prior_NEX_mu_inter_Nc] matrix[num_inter, num_inter] prior_NEX_mu_inter_sigma_L = rep_array(diag_matrix(rep_vector(1, num_inter)), prior_NEX_mu_inter_Nc);
  array[num_strata, num_comp, max_int(to_array_1d(prior_EX_tau_comp_Nc))] matrix[2, 2] prior_EX_tau_comp_sigma_L = rep_array(diag_matrix([1, 1]'), num_strata, num_comp, max_int(to_array_1d(prior_EX_tau_comp_Nc)));
  array[num_strata, max_int(prior_EX_tau_inter_Nc)] matrix[num_inter, num_inter] prior_EX_tau_inter_sigma_L = rep_array(diag_matrix(rep_vector(1, num_inter)), num_strata, max_int(prior_EX_tau_inter_Nc));
  // marginal means over the mixtures which are needed in case tau is
  // fixed to the known mean
  array[num_strata, num_comp] vector[2] prior_EX_tau_comp_mean = rep_array([0, 0]', num_strata, num_comp);
  array[num_strata] vector[num_inter] prior_EX_tau_inter_mean = rep_array(rep_vector(0, num_inter), num_strata);

  // convert the covariance matrices for all given mixture normals to
  // cholesky factors
  for (j in 1:num_comp) {
    for (k in 1:max_int(prior_EX_mu_comp_Nc)) {
      prior_EX_mu_comp_sigma_L[j,k] = block_cholesky_decompose(prior_EX_mu_comp_sigma[j,k]);
    }
    for (k in 1:max_int(prior_NEX_mu_comp_Nc)) {
      prior_NEX_mu_comp_sigma_L[j,k] = block_cholesky_decompose(prior_NEX_mu_comp_sigma[j,k]);
    }
    for (k in 1:max_int(to_array_1d(prior_EX_tau_comp_Nc))) {
      for (s in 1:num_strata) {
        prior_EX_tau_comp_sigma_L[s,j,k] = block_cholesky_decompose(prior_EX_tau_comp_sigma[s,j,k]);
      }
    }
  }
  
  for (k in 1:prior_EX_mu_inter_Nc) {
    prior_EX_mu_inter_sigma_L[k] = block_cholesky_decompose(prior_EX_mu_inter_sigma[k]);
  }
  for (k in 1:prior_NEX_mu_inter_Nc) {
    prior_NEX_mu_inter_sigma_L[k] = block_cholesky_decompose(prior_NEX_mu_inter_sigma[k]);
  }
  for (s in 1:num_strata) {
    for (k in 1:max_int(to_array_1d(prior_EX_tau_inter_Nc))) {
      prior_EX_tau_inter_sigma_L[s,k] = block_cholesky_decompose(prior_EX_tau_inter_sigma[s,k]);
    }
  }

  for (s in 1:num_strata) {
    for (j in 1:num_comp) {
      for (k in 1:prior_EX_tau_comp_Nc[s,j]) {
        prior_EX_tau_comp_mean[s,j] += prior_EX_tau_comp_w[s,j,k] * prior_EX_tau_comp_m[s,j,k];
      }
    }
    for (k in 1:prior_EX_tau_inter_Nc[s]) {
      prior_EX_tau_inter_mean[s] += prior_EX_tau_inter_w[s,k] .* prior_EX_tau_inter_m[s,k];
    }
  }

  array[num_EXNEX_comp, num_mix_dim == 0 ? 0 : power_int(2, num_mix_dim - 1)] int<lower=1,
                                                                    upper=num_mix_comp> mix_is_EX_beta;
  array[num_EXNEX_inter, num_mix_dim == 0 ? 0 : power_int(2, num_mix_dim - 1)] int<lower=1,
                                                                    upper=num_mix_comp> mix_is_EX_eta;
  array[num_mix_comp, num_comp] int<lower=1, upper=2> mix_idx_beta;
  array[num_mix_comp, num_inter] int<lower=1, upper=2> mix_idx_eta;
  // number of observations per group
  array[num_groups] int<lower=0, upper=num_obs> num_obs_group = count_elems(group,
                                                                    seq_int(1,
                                                                    num_groups));
  // number of cases per group
  array[num_groups] int<lower=0> num_cases_group = rep_array(0, num_groups);
  // indices for each group
  array[num_groups, max(num_obs_group)] int<lower=0, upper=num_obs> group_obs_idx = rep_array(0,
                                                                    num_groups,
                                                                    max(num_obs_group));
  array[num_groups] vector<upper=0>[num_mix_comp] mix_log_weight;
  vector[num_groups] log_normfactor_group = rep_vector(0, num_groups);
  
  // determine for each group the set of indices which belong to it
  for (g in 1 : num_groups) {
    int i = 1;
    for (o in 1 : num_obs) {
      if (group[o] == g) {
        group_obs_idx[g, i] = o;
        i = i + 1;
      }
    }
  }
  
  // check that within each group the stratum does not change which
  // would violate the nested structure
  for (g in 1 : num_groups) {
    int group_size = num_obs_group[g];
    array[group_size] int obs_gidx = group_obs_idx[g, 1 : group_size];
    if (cardinality_int(stratum[obs_gidx]) > 1) {
      reject("Group ", g, " is assigned to multiple strata.");
    }
  }
  
  for (j in 1 : num_comp) {
    vector[num_obs] X_comp_intercept = X_comp[j,  : , 1];
    if (cardinality_vector(X_comp_intercept) > 1 || X_comp[j, 1, 1] != 1.0) {
      reject("Compound (", j, ") design matrix must have an intercept.");
    }
  }
  
  if (num_inter > 0) {
    vector[num_obs] X_inter_intercept = X_inter[ : , 1];
    if (cardinality_vector(X_inter_intercept) == 1 && X_inter[1, 1] == 1.0) {
      print("INFO: Interaction design matrix appears to have an intercept, which is unexpected.");
    }
  }
  
  // NOTE: Non-centered parametrization is hard-coded
  
  for (i in 1 : num_obs) {
    n[i] = r[i] + nr[i];
    log_normfactor_group[group[i]] += lchoose(n[i], r[i]);
  }
  
  // count number of cases per group
  for (g in 1 : num_groups) {
    int group_size = num_obs_group[g];
    array[group_size] int obs_gidx = group_obs_idx[g, 1 : group_size];
    num_cases_group[g] = sum(n[obs_gidx]);
  }
  
  {
    array[num_obs] int finite_cov_sum = rep_array(0, num_obs);
    for (j in 1 : num_comp) {
      for (i in 1 : num_obs) {
        finite_cov[j, i] = !is_inf(X_comp[j, i, 1])
                           && !is_inf(X_comp[j, i, 2]) ? 1 : 0;
        finite_cov_sum[i] = finite_cov_sum[i] + finite_cov[j, i];
      }
    }
    for (i in 1 : num_obs) {
      if (finite_cov_sum[i] == 0) {
        reject("No finite covariates for observation ", i);
      }
    }
  }
  
  print("Number of groups: ", num_groups);
  print("Number of strata: ", num_strata);
  print("EXNEX enabled for compounds ", num_EXNEX_comp, "/", num_comp,
        ":    ", prior_is_EXNEX_comp);
  print("EXNEX enabled for interactions ", num_EXNEX_inter, "/", num_inter,
        ": ", prior_is_EXNEX_inter);
  print("EXNEX mixture dimensionality ", num_mix_dim, " leads to ",
        num_mix_comp, " combinations.");
  
  print("Observation => group assignment:");
  for (g in 1 : num_groups) {
    print("Group ", g, ": ", group_obs_idx[g, 1 : num_obs_group[g]]);
  }
  
  print("");
  print("Group => stratum assignment:");
  for (g in 1 : num_groups) {
    print(g, " => ", group_stratum_cid[g]);
  }
  
  print("Prior distribution on tau parameters:");
  if (prior_tau_dist == 0) {
    print("Fixed");
  } else if (prior_tau_dist == 1) {
    print("Log-Normal");
  } else if (prior_tau_dist == 2) {
    print("Truncated Normal");
  }
  
  if (prior_PD) {
    print("Info: Sampling from prior predictive distribution.");
  }
  
  for (g in 1 : num_groups) {
    mix_log_weight[g] = rep_vector(0.0, num_mix_comp);
  }
  
  // here we configure the different mixture possibilities since we
  // are not exhaustivley using all combinations those elements which
  // are fixed are filled in as a second step.
  for (i in 1 : num_mix_comp) {
    array[num_mix_dim] int mix_ind_base = decimal2base(i - 1, num_mix_dim, 2);
    array[num_comp + num_inter] int mix_ind;
    for (j in 1 : num_mix_dim) {
      // move 0/1 coding to 1/2
      mix_ind_base[j] += 1;
    }
    
    {
      int k = 1;
      for (j in 1 : num_comp) {
        // if the component is EXNEX, then its status is controlled by
        // the moving configuration; otherwise it is always EX
        if (prior_is_EXNEX_comp[j]) {
          mix_ind[j] = mix_ind_base[k];
          k += 1;
        } else {
          mix_ind[j] = 1;
        }
      }
    }
    {
      int k = 1;
      for (j in 1 : num_inter) {
        // if the interaction is EXNEX, then its status is controlled by
        // the moving configuration; otherwise it is always EX
        if (prior_is_EXNEX_inter[j]) {
          mix_ind[num_comp + j] = mix_ind_base[num_EXNEX_comp + k];
          k += 1;
        } else {
          mix_ind[num_comp + j] = 1;
        }
      }
    }
    
    for (g in 1 : num_groups) {
      // EX == 1 / NEX == 2
      // prior weights
      for (j in 1 : num_comp) {
        if (prior_is_EXNEX_comp[j]) {
          mix_log_weight[g, i] += mix_ind[j] == 1
                                  ? log(prior_EX_prob_comp[g, j])
                                  : log1m(prior_EX_prob_comp[g, j]);
        }
      }
      for (j in 1 : num_inter) {
        if (prior_is_EXNEX_inter[j]) {
          mix_log_weight[g, i] += mix_ind[num_comp + j] == 1
                                  ? log(prior_EX_prob_inter[g, j])
                                  : log1m(prior_EX_prob_inter[g, j]);
        }
      }
      
      // index configuration
      mix_idx_beta[i] = mix_ind[1 : num_comp];
      mix_idx_eta[i] = mix_ind[num_comp + 1 : num_comp + num_inter];
    }
  }
  
  // index vectors which indicate whenever a given parameter is EX for
  // the configuration number
  {
    int i = 1;
    for (j in 1 : num_comp) {
      if (prior_is_EXNEX_comp[j]) {
        mix_is_EX_beta[i] = which_elem(mix_idx_beta[ : , j], 1);
        i += 1;
      }
    }
  }
  {
    int i = 1;
    for (j in 1 : num_inter) {
      if (prior_is_EXNEX_inter[j]) {
        mix_is_EX_eta[i] = which_elem(mix_idx_eta[ : , j], 1);
        i += 1;
      }
    }
  }
}
parameters {
  // the first 1:num_groups parameters are EX modelled while the
  // num_groups+1:2*num_groups are NEX
  array[2 * num_groups, num_comp] vector[2] log_beta_raw;
  array[2 * num_groups] vector[num_inter] eta_raw;
  
  // hierarchical priors
  array[num_comp] vector[2] mu_log_beta;
  // for differential discounting we allow the tau's to vary by
  // stratum (but not the means)
  array[num_strata, num_comp] vector<lower=0>[2] tau_log_beta_raw;
  array[num_comp] cholesky_factor_corr[2] L_corr_log_beta;
  
  vector[num_inter] mu_eta;
  array[num_strata] vector<lower=0>[num_inter] tau_eta_raw;
  cholesky_factor_corr[num_inter] L_corr_eta;
}
transformed parameters {
  array[2 * num_groups, num_comp] vector[2] beta;
  array[2 * num_groups] vector[num_inter] eta;
  array[num_strata, num_comp] vector<lower=0>[2] tau_log_beta;
  array[num_strata] vector<lower=0>[num_inter] tau_eta;
  
  // in the case of fixed tau's we fill them in here
  if (prior_tau_dist == 0) {
    tau_log_beta = prior_EX_tau_comp_mean;
    tau_eta = prior_EX_tau_inter_mean;
  } else {
    tau_log_beta = tau_log_beta_raw;
    tau_eta = tau_eta_raw;
  }
  
  // EX parameters which vary by stratum which is defined by the group
  {
    array[num_strata, num_comp] matrix[2,2] L_log_beta;
    array[num_strata] matrix[num_inter, num_inter] L_eta;
    
    for (s in 1 : num_strata) {
      for (j in 1 : num_comp) {
        L_log_beta[s, j] = diag_pre_multiply(tau_log_beta[s, j],
                                             L_corr_log_beta[j]);
      }
      if (num_inter > 0) {
        L_eta[s] = diag_pre_multiply(tau_eta[s], L_corr_eta);
      }
    }
    
    for (g in 1 : num_groups) {
      int s = group_stratum_cid[g];
      for (j in 1 : num_comp) {
        beta[g, j] = mu_log_beta[j]
                     + L_log_beta[s,j] * log_beta_raw[g, j];
      }
      if (num_inter > 0) {
        eta[g] = mu_eta + L_eta[s] * eta_raw[g];
      }
    }
  }
  
  // NEX parameters
  beta[num_groups + 1 : 2 * num_groups] = log_beta_raw[num_groups + 1 : 2
                                                                    * num_groups];
  eta[num_groups + 1 : 2 * num_groups] = eta_raw[num_groups + 1 : 2
                                                                  * num_groups];
  
  // exponentiate the slope parameter to force positivity
  for (g in 1 : 2 * num_groups) {
    for (j in 1 : num_comp) {
      beta[g, j, 2] = exp(beta[g, j, 2]);
    }
  }
}
model {
  if (!prior_PD) {
    if (num_mix_comp == 1) {
      // no mixture model case, EXNEX off => use vectorization accross
      // data-rows
      vector[num_obs] theta;
      for (g in 1 : num_groups) {
        int s = group_stratum_cid[g];
        int group_size = num_obs_group[g];
        array[group_size] int obs_gidx = group_obs_idx[g, 1 : group_size];
        theta[obs_gidx] = blrm_logit_fast(obs_gidx, n, X_comp, finite_cov,
                                          X_inter, beta[g], eta[g]);
                                          
      }
      
      r ~ binomial_logit(n, theta); 
    } else {
      vector[num_groups] log_lik;
      // loop over the data by group; nested into that we have to
      // loop over the different mixture configurations
      for (g in 1 : num_groups) {
        int s = group_stratum_cid[g];
        int group_size = num_obs_group[g];
        array[group_size] int obs_gidx = group_obs_idx[g, 1 : group_size];
        if (num_cases_group[g] != 0) {
          // lpmf for each mixture configuration
          vector[num_mix_comp] mix_ll = blrm_mix_lupmf_comp(// subset data
                                                            g, num_groups,
                                                            obs_gidx, r, n,
                                                            X_comp,
                                                            finite_cov,
                                                            X_inter,
                                                            // select EX+NEX of this group
                                                            beta,
                                                            mix_idx_beta,
                                                            eta, mix_idx_eta)
                                        // prior weight for each component
                                        + mix_log_weight[g];
          // finally add the sum (on the natural scale) as log to the target
          // log density
          log_lik[g] = log_sum_exp(mix_ll);
        } else {
          // num_cases_group[g] == 0 => log_lik = 0
          log_lik[g] = 0.0;
        }
      }
      target += sum(log_lik);
    }
  }
  
  // EX part: hyper-parameters priors for hierarchical priors
  for (j in 1 : num_comp) {
    mu_log_beta[j] ~ mixmvnorm(prior_EX_mu_comp_Nc[j], prior_EX_mu_comp_w[j], prior_EX_mu_comp_m[j], prior_EX_mu_comp_sigma_L[j]);
    
    for (s in 1 : num_strata) {
      tau_log_beta_raw[s, j] ~ mixmv_tau_prior(prior_tau_dist, prior_EX_tau_comp_Nc[s,j], prior_EX_tau_comp_w[s,j], prior_EX_tau_comp_m[s,j], prior_EX_tau_comp_sigma_L[s,j]);
    }
    L_corr_log_beta[j] ~ lkj_corr_cholesky(prior_EX_corr_eta_comp[j]);
  }
  
  mu_eta ~ mixmvnorm(prior_EX_mu_inter_Nc, prior_EX_mu_inter_w, prior_EX_mu_inter_m, prior_EX_mu_inter_sigma_L);
  for (s in 1 : num_strata) {
    tau_eta_raw[s] ~ mixmv_tau_prior(prior_tau_dist, prior_EX_tau_inter_Nc[s], prior_EX_tau_inter_w[s], prior_EX_tau_inter_m[s], prior_EX_tau_inter_sigma_L[s]);
  }
  
  if (num_inter > 0) {
    L_corr_eta ~ lkj_corr_cholesky(prior_EX_corr_eta_inter);
  } 
  
  
  // hierarchical priors NCP
  for (g in 1 : num_groups) {
    for (j in 1 : num_comp) {
      log_beta_raw[g, j] ~ std_normal();
    }
    
    eta_raw[g] ~ std_normal();
  }
  
  // NEX priors (always uncorrelated)
  for (g in num_groups + 1 : 2 * num_groups) {
    for (j in 1 : num_comp) {
      // vectorized over intercept and slope
      log_beta_raw[g, j] ~ mixmvnorm(prior_NEX_mu_comp_Nc[j], prior_NEX_mu_comp_w[j], prior_NEX_mu_comp_m[j], prior_NEX_mu_comp_sigma_L[j]);
    }
    eta_raw[g] ~ mixmvnorm(prior_NEX_mu_inter_Nc, prior_NEX_mu_inter_w, prior_NEX_mu_inter_m, prior_NEX_mu_inter_sigma_L);
  }
}
generated quantities {
  matrix[num_groups, num_comp] beta_EX_prob;
  matrix[num_groups, num_inter] eta_EX_prob;
  array[num_groups, num_comp] vector[2] beta_group;
  array[num_groups] vector[num_inter] eta_group;
  vector[num_groups] log_lik_group;
  vector[num_comp] rho_log_beta;
  matrix[num_inter, num_inter] Sigma_corr_eta = multiply_lower_tri_self_transpose(L_corr_eta);
  array[sample_map ? num_strata : 0, num_comp] vector[2] map_log_beta;
  array[sample_map ? num_strata : 0] vector[num_inter] map_eta;
  
  for (j in 1 : num_comp) {
    matrix[2, 2] Sigma_corr_log_beta = multiply_lower_tri_self_transpose(L_corr_log_beta[j]);
    rho_log_beta[j] = Sigma_corr_log_beta[2, 1];
  }
  
  // posterior EX weights per group
  for (g in 1 : num_groups) {
    int group_size = num_obs_group[g];
    array[group_size] int obs_gidx = group_obs_idx[g, 1 : group_size];
    // lpmf for each mixture configuration
    vector[num_mix_comp] mix_ll = (prior_PD
                                   ? rep_vector(0.0, num_mix_comp)
                                   : blrm_mix_lupmf_comp(g, num_groups,
                                                         obs_gidx, r, n, X_comp,
                                                         finite_cov, X_inter,
                                                         beta, mix_idx_beta,
                                                         eta, mix_idx_eta))
                                  + mix_log_weight[g];
    real log_norm = log_sum_exp(mix_ll);
    vector[num_mix_comp] log_EX_prob_mix = mix_ll - log_norm;
    int mix_config_ind = categorical_rng(exp(log_EX_prob_mix));
    array[num_comp] int mix_beta_config = mix_idx_beta[mix_config_ind];
    array[num_inter] int mix_eta_config = mix_idx_eta[mix_config_ind];
    
    log_lik_group[g] = log_norm + log_normfactor_group[g];
    
    // marginalize & pick group specific parameters which have been sampled
    {
      int i = 1;
      for (j in 1 : num_comp) {
        if (prior_is_EXNEX_comp[j]) {
          beta_EX_prob[g, j] = exp(log_sum_exp(log_EX_prob_mix[mix_is_EX_beta[i]]));
          i += 1;
        } else {
          beta_EX_prob[g, j] = 1.0;
        }
        beta_group[g, j] = beta[g
                                + (mix_beta_config[j] == 1 ? 0 : num_groups), j];
      }
    }
    {
      int i = 1;
      for (j in 1 : num_inter) {
        if (prior_is_EXNEX_inter[j]) {
          eta_EX_prob[g, j] = exp(log_sum_exp(log_EX_prob_mix[mix_is_EX_eta[i]]));
          i += 1;
        } else {
          eta_EX_prob[g, j] = 1.0;
        }
        eta_group[g, j] = eta[g + (mix_eta_config[j] == 1 ? 0 : num_groups), j];
      }
    }
  }

  if (sample_map) {
    for (s in 1:num_strata) {
      for (j in 1 : num_comp) {
        map_log_beta[s, j] = multi_normal_cholesky_rng(mu_log_beta[j],
                                                       diag_pre_multiply(tau_log_beta[s, j],
                                                                         L_corr_log_beta[j]));
      }
      if (num_inter > 0) {
        map_eta[s] = multi_normal_cholesky_rng(mu_eta,
                                               diag_pre_multiply(tau_eta[s],
                                                                 L_corr_eta));
      }
    }
  }
}

