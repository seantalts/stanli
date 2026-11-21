// CCC-Parameterization
functions { 
matrix cov2cor(matrix C){
    int dm = rows(C);
    matrix[dm,dm] s;
    matrix[dm,dm] R;
    s = diag_matrix( 1 ./ sqrt(diagonal(C)) );
    R = s*C*s ; //quad_form_diag(C, s);
    return R;
}
/*
  Computes jacobian for the a_b_scale transform.
  @param a Lower limit.
  @param b Upper limit.
  @param value The unconstrained value to be transformed.
  @return log jacobian contribution.
 */
real a_b_scale_jac(real a, real b, real value) {
  real invlogit_value = inv_logit(value);
  real out = log(b - a) + log(invlogit_value) + log1m(invlogit_value);
  return(out);
}

/*
  Takes unconstrained R value, transforms to be between a and b.
  @param a Lower limit.
  @param b Upper limit.
  @param value The unconstrained value to transform
  @return real value between a and b.
 */
real a_b_scale(real a, real b, real value) {
  return(a + (b - a) * inv_logit(value));
}

/*
  Compute the upper limit of b_h from a_h, given that sum(a_h) + sum(b_h) < 1
  This works for b_h given a_h, or a_h given b_h. They yield identical models, assuming a dual constraint, such that 0 < sum(a_h) < 1 and 0 < sum(b_h) < 1 - sum(a_h), or vice-versa.
  @param a_h Array of vectors [nt, Q].
  @return vector[nt] Upper limits such that sum(b_h{k}) < UpperLimit{k} = 1 - sum(a_h{k})
 */
vector upper_limits(array[] vector a_h) {
  int nt = num_elements(a_h[1]);
  int Q = size(a_h);
  vector[nt] a_h_sums;
  vector[nt] out;
  for(k in 1:nt) {
    a_h_sums[k] = sum(a_h[1:Q, k]);
    out[k] = 1 - a_h_sums[k];
    if(out[k] <= 0) {out[k] = .00001;}
  }

  return(out);
}

/*
  Transform unconstrained sums to (0, upperLimits).
  @param b_h_sum_s A vector [nt] of unconstrained sums.
  @param upperLimits A vector [nt] of the upper limits.
  @return vector[nt] of values between [0, upperLimits].
 */
vector raw_sum_to_b_h_sum(vector b_h_sum_s, vector upperLimits) {
  int nt = num_elements(upperLimits);
  vector[nt] out;
  for(k in 1:nt) {
    out[k] = a_b_scale(0, upperLimits[k], b_h_sum_s[k]);
  }
  return(out);
}
/*
  Transform the array of b_h simplexes to b_h.
  
  The dual constraint is such that 0 < sum(a_h) + sum(b_h) < 1, for each timeseries.
  We assume sum(a_h) is [0,1], then sum(b_h) is [0, 1 - sum(a_h)].
  The sums are estimated for a_h and b_h.
  Then the a_h and b_h simplexes are scaled by the sums to produce a_h and b_h.

  @param b_h_simplex Array [nt] of simplexes [P].
  @param b_h_sum vector [nt] of the value to which the b_h's should be summed.
  @return An array [P] of vectors [nt].
 */
array[] vector simplex_to_bh(array[] vector b_h_simplex, vector b_h_sum) {
  int nt = size(b_h_simplex);
  int P = num_elements(b_h_simplex[1]);
  array[P] vector[nt] b_h;
  for(k in 1:nt) {
    b_h[1:P, k] = to_array_1d(b_h_simplex[k] * b_h_sum[k]);
  }
  return(b_h);
}
}
data {
int<lower=2> T; // lengh of time series
int<lower=2> nt;    // number of time series
int<lower=1> Q; // MA component in MGARCH(P,Q), matrix A
int<lower=1> P; // AR component in MGARCH(P,Q), matrix B
array[T] vector[nt] rts;  // multivariate time-series
array[T] vector[nt] xC;  // time-varying predictor for constant variance
int<lower=0, upper=1> distribution; // 0 = Normal; 1 = student_t
int<lower=0, upper=2> meanstructure; // Select model for location
}
transformed data {
  // Obtain mean and sd over TS for prior in arma process phi0
  vector[nt] rts_m;
  vector[nt] rts_sd;

// Check whether xC contains a predictor or not.
array[T] matrix[nt, nt] xC_m;
int<lower = 0> xC_marker = 0;
real<lower = 0> cp;

for( t in 1:T ){
  xC_m[t] = diag_matrix( xC[t] );
  // add a variable that notes if xC is null or actually a predictor
  cp = sum( xC_m[t]' * xC_m[t] );
  if( cp != 0 )
    xC_marker = xC_marker + 1;
 }

  if( meanstructure == 0 ){
    for ( i in 1:nt ){
      rts_m[i] = mean(rts[,i]);
      rts_sd[i] = sd(rts[,i]);
    }
  } else if (meanstructure == 1 || meanstructure == 2){
      // set rts_m to first element in ts
      for ( i in 1:nt ){
	rts_m[i] = rts[1,i];
	rts_sd[i] = sd(rts[,i]);
      }
    }
}
parameters {
 // ARMA parameters 
vector[nt] phi0; 
// gqs() cant not deal with ? yet - as long as that's not fixed
// estimate phi and theta anyways
//matrix[meanstructure ? nt : 0, meanstructure ? nt : 0 ] phi;
//matrix[meanstructure ? nt : 0, meanstructure ? nt : 0 ] theta;
matrix<lower = -1, upper = 1>[nt,nt] phi;
matrix<lower = -1, upper = 1>[nt,nt] theta;
  // predictor for H
// Predictors for H
//cov_matrix[ xC_marker >= 1 ? nt : 0 ] beta;
// gqs() cant currently handle ? opeartor do this instead:
vector[nt] beta;

  // GARCH h parameters on variance metric
  vector[nt] c_h; 
  // vector<lower=0, upper = 1 >[nt] a_h[Q];
  array[nt] simplex[Q] a_h_simplex;
  vector<lower=0, upper = 1>[nt] a_h_sum;
  array[nt] simplex[P] b_h_simplex;
  vector[nt] b_h_sum_s;
  // vector<lower=0, upper = 1 >[nt] b_h[P]; // TODO actually: 1 - a_h, across all Q and P...

  // GARCH constant correlation 
  corr_matrix[nt] R;

  // D1 init
  vector<lower = 0>[nt] D1_init;

  // DF constant nu for student t
  real< lower = 2 > nu;

}
transformed parameters {
  array[T] cov_matrix[nt] H;
  array[T-1] vector[nt] rr;
  array[T] vector[nt] mu;
  array[T] vector[nt] D;
  array[nt] real<lower = 0> vd;
  array[nt] real<lower = 0> ma_d;
  array[nt] real<lower = 0> ar_d;
  array[Q] vector<lower=0, upper = 1>[nt] a_h = simplex_to_bh(a_h_simplex, a_h_sum);
  vector[nt] UPs = upper_limits(a_h);
  vector[nt] ULs = raw_sum_to_b_h_sum(b_h_sum_s, UPs);
  array[P] vector<lower = 0, upper = 1>[nt] b_h = simplex_to_bh(b_h_simplex, ULs);
  // Initialize t=1
  // Check "Order Sensitivity and Repeated Variables" in stan reference manual
  mu[1,] = phi0;
  //u[1,] = diagonal(sigma1);  
  D[1,] = D1_init;
  H[1,] = quad_form_diag(R, D[1,]);  // H = DRD; 
  // iterations geq 2
  for (t in 2:T){

    // Meanstructure model:
if( meanstructure == 0 ){
  // Constant
  mu[t, ] = phi0;
 } else if( meanstructure == 1 ){
  // arma11
  mu[t, ] = phi0 + phi * rts[t-1, ] + theta * (rts[t-1, ] - mu[t-1,]) ;
 } else if( meanstructure == 2 ){
  // VAR1
  mu[t, ] = phi0 + phi * rts[t-1, ];
 }
    
    // scale: SD's of D
    for (d in 1:nt) {
      vd[d] = 0.0;
      ma_d[d] = 0.0;
      ar_d[d] = 0.0;
      // MA component
      for (q in 1:min( t-1, Q) ) {
	rr[t-q, d] = square( rts[t-q, d] - mu[t-q, d] );
	ma_d[d] = ma_d[d] + a_h[q, d]*rr[t-q, d] ;
      }
      for (p in 1:min( t-1, P) ) {
	ar_d[d] = ar_d[d] + b_h[p, d]*D[t-p, d]^2;
      }
      if ( xC_marker >= 1) {
	vd[d] = exp( c_h[d] + beta[d] * xC[t, d] ) + ma_d[d] + ar_d[d];
      } else if ( xC_marker == 0) {
      	vd[d] = exp( c_h[d] )  + ma_d[d] + ar_d[d];
      }
      D[t, d] = sqrt( vd[d] );
    }
  H[t,] = quad_form_diag(R, D[t,]);  // H = DRD;
  }
}

model {
  // priors
  for(k in 1:nt) {
    ULs[k] ~ uniform(0, UPs[k]);
    target += a_b_scale_jac(0, ULs[k], b_h_sum_s[k]);
  }
  to_vector(beta) ~ std_normal( );
  to_vector(c_h) ~ std_normal( );
  if ( distribution == 1 )
    nu ~ normal( nt, 50 );
  to_vector(D1_init) ~ lognormal(0, 1);
  to_vector(theta) ~ std_normal( );
  to_vector(phi) ~ std_normal( );
  phi0 ~ multi_normal(rts_m, diag_matrix( rts_sd ) );
  R ~ lkj_corr( 1 );
  // likelihood
  if ( distribution == 0 ) {
    for(t in 1:T){
      rts[t,] ~ multi_normal(mu[t,], H[t,]);
      //target += multi_normal_lpdf( rts[t, ] | mu[t,], H[t,]);
    }
  } else if ( distribution == 1 ) {
    for(t in 1:T){
      rts[t,] ~ multi_student_t(nu, mu[t,], H[t,]);
      //target += multi_student_t_lpdf( rts[t, ] | nu, mu[t,], H[t,]);
    }
  }
}
generated quantities {
  matrix[nt,T] rts_out;
  array[T] real log_lik;
  array[T] corr_matrix[nt] corH;
  vector<lower=0>[nt] c_h_var = exp(c_h);

  // retrodict
// retrodict given distribution type
if ( distribution == 0 ){ 
  for (t in 1:T) {
    rts_out[,t] = multi_normal_rng(mu[t,], H[t,]);
    corH[t,] = cov2cor(H[t,]);
    log_lik[t] = multi_normal_lpdf(rts[t,] | mu[t,], H[t,]);
  }
 } else if ( distribution == 1 ) {
  for (t in 1:T) {
    rts_out[,t] = multi_student_t_rng(nu, mu[t,], H[t,]);
    corH[t,] = cov2cor(H[t,]);
    log_lik[t] = multi_student_t_lpdf(rts[t,] | nu, mu[t,], H[t,]);
  }
 }
  
}
