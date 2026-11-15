/* Code adapted from Virgile Baudrot
https://github.com/virgile-baudrot/gutsRstan */

/*
    BeeGUTS is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    BeeGUTS is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with BeeGUTS.  If not, see <http://www.gnu.org/licenses/>.
*/

functions {

/* for multiple .stan files */

/* bisectioning search of the index i such that sorted[i] < x < sorted[i+1]
  this come from Sebastian Weber
  */
int find_interval_elem(real x, vector sorted, int start_ind){
      int res;
      int N;
      int max_iter;
      real left;
      real right;
      int left_ind;
      int right_ind;
      int iter;

      N = num_elements(sorted);

      if(N == 0) return(0);

      left_ind  = start_ind;
      right_ind = N;

      max_iter = 100 * N;
      left  = sorted[left_ind ] - x;
      right = sorted[right_ind] - x;

      if(0 <= left)  return(left_ind-1);
      if(0 == right) return(N-1);
      if(0 >  right) return(N);

      iter = 1;
      while((right_ind - left_ind) > 1  && iter != max_iter) {
        int mid_ind;
        real mid;
        // is there a controlled way without being yelled at with a warning?
        mid_ind = (left_ind + right_ind) %/% 2;
        mid = sorted[mid_ind] - x;
        if (mid == 0) return(mid_ind-1);
        if (left  * mid < 0) { right = mid; right_ind = mid_ind; }
        if (right * mid < 0) { left  = mid; left_ind  = mid_ind; }
        iter = iter + 1;
      }
      if(iter == max_iter) print("Maximum number of iterations reached.");
      return(left_ind);
 }
 /*    Function for linear interpolation*/
real linearInterp( real t_x, real t_before, real t_after, real y_before, real y_after){
    real linInterp_hat;

    linInterp_hat = y_before + (t_x - t_before) * (y_after - y_before)/(t_after-t_before);

    return(linInterp_hat);
}

 /*    Function for loglogistic law */
real loglogistic_2_lpdf(real y_hat, real mw, real beta) {
      return log(beta) - log(mw) + (beta - 1) * (log(y_hat) - log(mw)) -
             2 * log1p_exp(beta * (log(y_hat) - log(mw)));
}

real  loglogistic_2_lcdf(real y_hat, real mw, real beta){
  return -log1p_exp(-beta * (log(y_hat) - log(mw))) ;
}

real  loglogistic_2_lccdf(real y_hat, real mw, real beta){
  return -log1p_exp(beta * (log(y_hat) - log(mw))) ;
}

  vector TKTD_varSD( real t,
                     vector y,
                     array[] real theta,
                     array[] real x_r,
                     array[] int  x_i) {

    // - parameters
    real kd = theta[1];
    real zw = theta[2];
    real bw = theta[3];
    real hb = theta[4];

    // - new variables
    array[2] real max_zw; //
    vector[2] dy_dt; //

    // - latent variables
    int Nconc = x_i[1]; // Number of point measuring concentration

    vector[Nconc] tconc = to_vector(x_r[1:Nconc]);
    vector[Nconc] conc = to_vector(x_r[Nconc+1:2*Nconc]); // to take dose time which is in ts

    int pulse_index = find_interval_elem(t, tconc, 1); // pulse_index index

    // if_else statement
    real conc_linInterp = pulse_index != 0 ? linearInterp(t , tconc[pulse_index], tconc[pulse_index+1], conc[pulse_index], conc[pulse_index+1] ) : conc[1];
    //real conc_linInterp = linearInterp(t , tconc[pulse_index], tconc[pulse_index+1], conc[pulse_index], conc[pulse_index+1] );

    // - model
    dy_dt[1] =  kd * ( conc_linInterp - y[1]);

    max_zw[1] = 0;
    max_zw[2] = y[1] - zw;

    dy_dt[2] = bw * max(max_zw) + hb;

    return(dy_dt);
  }

  matrix solve_TKTD_varSD(array[] real y0, real t0, array[] real ts, array[] real theta, data array[] real tconc, data array[] real conc, data real relTol, data real absTol, int maxSteps){

    array[1] int x_i;
    x_i[1] = size(tconc);

    array[size(ts)] vector[2] ode_res
      = ode_rk45_tol(TKTD_varSD, to_vector(y0), t0, ts,
                         // additional control parameters for the solver: real rel_tol, real abs_tol, int max_num_steps
                         relTol, absTol, maxSteps, theta,
                         to_array_1d(append_row(to_vector(tconc), to_vector(conc))),
                         x_i);
    matrix[size(ts), 2] rtn;
    for(i in 1:size(ts)) {
      rtn[i] = transpose(ode_res[i]);
    }
    return rtn;
  }
}

data {

/* Code adapted from Virgile Baudrot
https://github.com/virgile-baudrot/gutsRstan */

// Number of datasets
int <lower=1> nDatasets;

// Number of groups
int<lower=1> nGroup; // Number of groups (one group is combination of one dataset and one treatment)
array[nGroup] int groupDataset; // Corresponding dataset for each group

// Concentration
int<lower=1> nData_conc; // length of data for concentration
array[nData_conc] real conc; // concentration
array[nData_conc] real tconc; // time of concentration

array[nGroup] int<lower=1> idC_lw; // e.g. 1 6 12 18
array[nGroup] int<lower=1> idC_up; // e.g. 6 12 18 24

// Survivors
int<lower=1> nData_Nsurv; // number of group: 4
array[nData_Nsurv] int Nsurv;
array[nData_Nsurv] int Nprec;
array[nData_Nsurv] real tNsurv; // time of Nbr survival

array[nGroup] int<lower=1> idS_lw; // e.g. 1 6 12 18
array[nGroup] int<lower=1> idS_up; // e.g. 6 12 18 24

// PRIORS
real hbMean_log10;
real hbSD_log10;
real kdMean_log10;
real kdSD_log10;

// Parameters for integration of differentiol equations
real relTol;
real absTol;
int maxSteps;

  /* PRIORS */
  real bwMean_log10;
  real bwSD_log10;
  real zwMean_log10;
  real zwSD_log10;

}
transformed data{

  array[2] real<lower=0> y0;

  array[nData_Nsurv] real tNsurv_ode; // time of Nbr survival to include in the ode !
  array[nData_conc] real tconc_ode; // time of Nbr survival to include in the ode !

  y0[1] = 0;
  y0[2] = 0;

  for(gr in 1:nGroup){
    tNsurv_ode[idS_lw[gr]:idS_up[gr]] = tNsurv[idS_lw[gr]:idS_up[gr]];
    tNsurv_ode[idS_lw[gr]] = tNsurv[idS_lw[gr]] + 1e-9 ; // to start ode integrator at 0
    tconc_ode[idC_lw[gr]:idC_up[gr]] = tconc[idC_lw[gr]:idC_up[gr]];
    tconc_ode[idC_lw[gr]] = tconc[idC_lw[gr]] + 1e-9 ; // to start ode integrator at 0
  }


}
parameters {

  array[3 + nDatasets] real sigma;

}
transformed parameters{

  real kd_log10 = kdMean_log10 + kdSD_log10 * sigma[1];
  real zw_log10 = zwMean_log10 + zwSD_log10 * sigma[2];
  real bw_log10 = bwMean_log10 + bwSD_log10 * sigma[3];
  array[nDatasets] real hb_log10;

  array[4] real<lower=0> param; //

  matrix[nData_Nsurv,2] y_hat;
  vector<lower=0, upper=1>[nData_Nsurv] Psurv_hat;
  vector<lower=0, upper=1>[nData_Nsurv] Conditional_Psurv_hat;


  param[1] = 10^kd_log10; // kd
  param[2] = 10^zw_log10; // zw
  param[3] = 10^bw_log10; // bw


  for(i in  1:nDatasets){
    hb_log10[i]  = hbMean_log10 + hbSD_log10 * sigma[3+i];
  }

  for(gr in 1:nGroup){
    param[4] = 10^hb_log10[groupDataset[gr]]; // hb

  /* initial time must be less than t0 = 0, so we use a very small close small number 1e-9 to at at time tNsurv and tconc */
    y_hat[idS_lw[gr]:idS_up[gr],1:2] = solve_TKTD_varSD(y0, 0, tNsurv_ode[idS_lw[gr]:idS_up[gr]], param, tconc_ode[idC_lw[gr]:idC_up[gr]], conc[idC_lw[gr]:idC_up[gr]], relTol, absTol, maxSteps);

    Psurv_hat[idS_lw[gr]:idS_up[gr]] = exp( - y_hat[idS_lw[gr]:idS_up[gr], 2]);

    for(i in idS_lw[gr]:idS_up[gr]){

      Conditional_Psurv_hat[i] =  i == idS_lw[gr] ? Psurv_hat[i] : Psurv_hat[i] / Psurv_hat[i-1] ;

    }
  }

}
model {

  target += normal_lpdf(sigma | 0, 1);

  for(gr in 1:nGroup){

    target += binomial_lpmf(Nsurv[idS_lw[gr]:idS_up[gr]] | Nprec[idS_lw[gr]:idS_up[gr]], Conditional_Psurv_hat[idS_lw[gr]:idS_up[gr]]);

    // Nsurv[idS_lw[gr]:idS_up[gr]] ~ binomial( Nprec[idS_lw[gr]:idS_up[gr]], Conditional_Psurv_hat[idS_lw[gr]:idS_up[gr]]);

  }
}
generated quantities {

/* Code adapted from Virgile Baudrot
https://github.com/virgile-baudrot/gutsRstan */

array[nData_Nsurv] int Nsurv_ppc;
array[nData_Nsurv] int Nsurv_sim;
array[nData_Nsurv] int Nsurv_sim_prec;

vector[nData_Nsurv] log_lik;

for(gr in 1:nGroup){
  /* binomial_rng function cannot be vectorized, so we need to use a loop*/
   for(i in idS_lw[gr]:idS_up[gr]){
     Nsurv_ppc[i] = binomial_rng(Nprec[i], Conditional_Psurv_hat[i]);

     Nsurv_sim_prec[i] = i == idS_lw[gr] ? Nprec[i] : Nsurv_sim[i-1] ;

     Nsurv_sim[i] = binomial_rng(Nsurv_sim_prec[i], Conditional_Psurv_hat[i]);

     log_lik[i] = binomial_lpmf(Nsurv[idS_lw[gr]:idS_up[gr]] | Nprec[idS_lw[gr]:idS_up[gr]], Conditional_Psurv_hat[idS_lw[gr]:idS_up[gr]]);
   }
}

}

