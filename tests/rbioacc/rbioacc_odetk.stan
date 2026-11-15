/*
    rbioacc is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    rbioacc is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with rbioacc.  If not, see <http://www.gnu.org/licenses/>.
*/
functions{
  
//functions {
int findfirst(real t, vector xt) {
  int i = 0 ;
  if(t == max(xt)){
    i = num_elements(xt) - 1 ;
    return i ;
  } else if(t < min(xt) || t > max(xt)){
    return i ;
  } else {
    while (t >= xt[i+1]){
      i = i+1 ;
    }
    return i ;
  }
}

real interpolate(real x, vector xpt, vector ypt){
  if(x >= min(xpt) && x <= max(xpt)){
    int idx = findfirst(x, xpt) ;
    return ypt[idx] + (x - xpt[idx]) * (ypt[idx+1] - ypt[idx]) / (xpt[idx+1] - xpt[idx]) ;
  } else{
    return 0.0 ;
  }
}
//}

array[] real odeTK(real t,      // time
             array[] real y,    // variables
             array[] real theta,
             array[] real x_r,
             array[] int x_i) {
  
  // parameters
  
  int lentp_rmNA = x_i[1] ;
  int lentp = x_i[2] ;
  int n_exp = x_i[3] ; 
  int n_out = x_i[4] ; 
  int n_met = x_i[5] ;

  array[n_exp] real ku = theta[1:n_exp] ;
  array[n_out] real ke = theta[(n_exp+1):(n_exp+n_out)] ;
  array[n_met] real km = theta[(n_exp+n_out+1):(n_exp+n_out+n_met)] ;
  array[n_met] real kem = theta[(n_exp+n_out+n_met+1):(n_exp+n_out+n_met+n_met)] ;
   
  // vector[1+n_met] dydt ;
  array[1+n_met] real dydt ;
  
  real tacc = x_r[1] ;
  // array[lentp_rmNA] real tp_rmNA = x_r[2:(lentp_rmNA+1)] ;
  vector[lentp_rmNA] tp_rmNA = to_vector(x_r[2:(lentp_rmNA+1)]) ;
  // WORK ONLY FOR ONE EXPOSURE PROFILE
  
  // array[lentp_rmNA] real Cexp_rmNA = x_r[(lentp_rmNA+2):(lentp_rmNA+2+lentp_rmNA)] ;
  vector[lentp_rmNA] Cexp_rmNA = to_vector(x_r[(lentp_rmNA+2):(lentp_rmNA+1+lentp_rmNA)]) ;

  // latent vairable
  real U ;
  real M ;
  real E ;
  
  if(n_met == 0){
    M = 0 ;
  } else{
    M = sum(km) ;
  }
  E = sum(ke) ;
  
  // model
  // U = ku[1] * interpolate(t, tp_rmNA, Cexp_rmNA[1:lentp_rmNA, 1]) ;
  U = ku[1] * interpolate(t, tp_rmNA, Cexp_rmNA) ;
  // if(n_exp >= 2){
  //   for(i in 2:n_exp){
  //     U = U +  ku[i] * interpolate(t, tp_rmNA, Cexp_rmNA[1:lentp_rmNA, i]) ;
  //   }
  // }
  if(t <= tacc){
    // Accumulation
    dydt[1] = U - (E + M) * y[1] ;
  } else{
    // Depuration
    dydt[1] = - (E + M) * y[1] ;
  }
  if(n_met > 0){
    for(i in 2:(n_met+1)){
      dydt[i] = km[i] * y[1] -  kem[i] * y[i] ;
    }
  }
  return(dydt) ;
}


// matrix solve_TK(
//   array[] real y0, real t0, array[] real ts, array[] real theta, array[] int n_int,
//   real tacc, array[] real tp_rmNA, array[] real Cexp_rmNA, array[] real odeParam){
//     
//     //vector[1+size(Cexp_rmNA) + size(tp_rmNA)] vector_r ;
//     array[1+size(Cexp_rmNA) + size(tp_rmNA)] real vector_r  ;
//     // array[1+size(tp_rmNA)] real vector_r  ;
//     
//     vector_r[1] = tacc ;
//     // vector_r[2:(size(Cexp_rmNA)+1)] = tp_rmNA ;
//     for(i in 1:size(tp_rmNA)){
//       vector_r[i+1] = tp_rmNA[i] ;
//     }
//     for(i in 1:size(Cexp_rmNA)){
//        vector_r[i+1+size(tp_rmNA)] = Cexp_rmNA[i] ;
//     }
//     
//     // vector_r[(size(Cexp_rmNA)+2):(size(Cexp_rmNA)+size(tp_rmNA)+1)] = Cexp_rmNA ;
//     
//     // append_row(to_vector(tp_rmNA), to_vector(Cexp_rmNA));
// 
//     return(to_matrix(
//       integrate_ode_rk45(odeTK, y0, t0, ts, theta,
//                          //to_array_1d(vector_r),
//                          vector_r,
//                          // to_array_1d(append_row(to_vector(vector_r), to_vector(Cexp_rmNA))),
//                          to_array_1d(n_int),
//                          // additional control parameters for the solver: real rel_tol, real abs_tol, int max_num_steps
//                          odeParam[1], odeParam[2], odeParam[3]))) ;
// 
//   }
// #include /include/TKodeSolver.stan
}

data {
  // Number of replicate
  int<lower=0> n_rep ;
  
  // Time points
  int<lower=0> lentp;
  array[lentp] real tp ;
  
  // Exposure profiles
  int<lower=0> n_exp ;
  int<lower=0> lentp_rmNA ;
  vector[lentp_rmNA] tp_rmNA ;
  matrix[lentp_rmNA, n_exp] Cexp_rmNA ;
  
  // Internal concentraion
  // Growth
  int<lower=0> n_out ;
  array[lentp, n_out, n_rep] real CGobs;

  // Metabolites
  int<lower=0> n_met ;
  array[lentp, n_met, n_rep] real Cmet ;

  //vector[1+n_met] y0;
  //real t0;

  real<lower=0> gmaxsup ;
  
  // TK accumulation / depuration
  int<lower=0> rankacc ;
  real<lower=0> tacc ;
  real<lower=0> C0 ;
  
  real unifMax ;
  int<lower=0> len_vt;
  vector[len_vt] vt ;
  
  // Parameters for integration of differentiol equations
  array[1+n_met] real y0;
  real t0 ;
  
  // real rel_tol;
  // real abs_tol;
  // int max_num_steps;
}
transformed data{
  array[1+lentp_rmNA+lentp_rmNA] real x_r ;
  array[5] int x_int ;
  
  x_r[1] = tacc ;
  
  for(i in 1:lentp_rmNA){
    x_r[1+i] = tp_rmNA[i] ;
  }
  for(i in 1:lentp_rmNA){
    x_r[i+lentp_rmNA+1] = Cexp_rmNA[i,1] ;
  }
  
  x_int[1] = lentp_rmNA ;
  x_int[2] = lentp ;
  x_int[3] = n_exp ;
  x_int[4] = n_out ;
  x_int[5] = n_met ;

}
parameters {
  vector[n_exp] log10ku ; // uptake
  vector[n_out] log10ke ;
  vector[n_met] log10km ;
  vector[n_met] log10kem ;
  
  array[n_out] real<lower=0> sigmaCGpred ; 
  vector<lower=0>[n_met] sigmaCmetpred ;
  
  array[n_out - 1] real<lower=0> gmax ;
  array[n_out -1] real<lower=0> G0;
  
}
transformed parameters{
  // PRIORS
  vector<lower=0>[n_exp] ku ;
  vector<lower=0>[n_out] ke ;
  vector<lower=0>[n_met] km ;
  vector<lower=0>[n_met] kem ;
  // with time-variable exposure profile,
  // exposure may have NA values
  matrix[lentp, n_exp] Cexp ;
  // little hack merging Cpred and Gpred
  matrix[lentp,n_out] CGpred ; 
  matrix[lentp,n_met] Cmetpred ;

  // array[5] int n_val ;
  // real theta ;

  array[lentp, 1+n_met] real y_sim ; 
  // matrix[lentp, 1+n_met] y_sim ;
  
  array[n_exp+n_out+n_met+n_met] real theta ;
  
  for(i in 1:n_exp){
    ku[i] = 10 ^ log10ku[i] ;
  }
  for(i in 1:n_out){
    ke[i] = 10 ^ log10ke[i] ;
  }
  for(i in 1:n_met){
    km[i] = 10 ^ log10km[i] ;
    kem[i] = 10 ^ log10kem[i] ;
  }
  
  // n_val[1] = lentp_rmNA ;
  // n_val[2] = lentp ;
  // n_val[3] = n_exp ;
  // n_val[4] = n_out ;
  // n_val[5] = n_met ;
 
  // y_sim = ode_rk45(odeTK, y0, t0, tp,lentp_rmNA,entp, n_exp,
  // n_out,n_met,n_val, tp_rmNA, Cexp_rmNA, tacc, ku, ke, km, kem) ;
  
  for(i in 1:n_exp){
    theta[i] = ku[i] ;
  }
   for(i in 1:n_out){
    theta[n_exp+i] = ke[i] ;
  }
   for(i in 1:n_met){
    theta[n_exp+n_out+i] = km[i] ;
  }
   for(i in 1:n_met){
    theta[n_exp+n_out+n_met+i] = kem[i] ;
  }

  y_sim = integrate_ode_rk45(odeTK, y0, t0, tp, theta, x_r, x_int) ;
  // y_sim = run(y0, t0, tp,lentp_rmNA,entp, n_exp,
  // n_out,n_met,n_val, tp_rmNA, Cexp_rmNA, tacc, ku, ke, km, kem) ;

  for(t in 1:lentp){
    CGpred[t,1] = y_sim[t, 1] ;
    for(i in 1:n_met){
      Cmetpred[t,i] = y_sim[t, i+1] ;
    }
  }
  // GROWTH
  if(n_out == 2){
    for(t in 1:lentp){
      CGpred[t, 2] = (G0[1] - gmax[1]) * exp(-ke[2] * tp[t]) + gmax[1] ;
    }
  }
}
model {
  // PRIORS
  target += uniform_lpdf(log10ku | -5, 5) ;
  target += uniform_lpdf(log10ke | -5, 5) ;
  target += uniform_lpdf(log10km | -5, 5) ;
  target += uniform_lpdf(log10kem | -5, 5) ;
  target += uniform_lpdf(sigmaCGpred[1] | 0, unifMax) ;
  target += uniform_lpdf(sigmaCmetpred | 0, unifMax) ;
  if(n_out == 2){
     target +=  uniform_lpdf(sigmaCGpred[2] | 0, unifMax) ;
     target +=  uniform_lpdf(gmax[1] | gmaxsup/6, gmaxsup) ;
     target +=  uniform_lpdf(G0[1] | 0, gmaxsup) ;
  }
  
  for(rep in 1:n_rep){
    // ACCUMULATION PHASE (0 =< t =< tacc) #
    for(t in 1:rankacc){
      // Parent compound
      if(!is_inf(CGobs[t,1,rep])){
        target += normal_lpdf(CGobs[t,1,rep] | CGpred[t, 1], sigmaCGpred[1]) ;
      }
      // Metabolites
      for(i in 1:n_met){
        if(!is_inf(Cmet[t,i,rep])){
          target += normal_lpdf(Cmet[t,i, rep] | Cmetpred[t,i], sigmaCmetpred[i]) ;
        }
      }
    }
    //DEPURATION PHASE (t > tacc)
    for(t in (rankacc+1):lentp){
      // Parent compound
      if(!is_inf(CGobs[t,1,rep])){
        target += normal_lpdf(CGobs[t,1,rep] | CGpred[t, 1], sigmaCGpred[1]) ;
      }
      // Metabolites
      for(i in 1:n_met){
        if(!is_inf(Cmet[t,i,rep])){
          target += normal_lpdf(Cmet[t,i,rep] | Cmetpred[t,i], sigmaCmetpred[i]) ;
        }
      }
    }
    // GROWTH
    if(n_out == 2){
      for(t in 1:lentp){
        if(!is_inf(CGobs[t,2,rep])){
          target += normal_lpdf(CGobs[t,2,rep] | CGpred[t,2], sigmaCGpred[2]) ;
        }
      }
    }
  }
}
