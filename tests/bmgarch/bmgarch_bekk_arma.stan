// BEKK-Parameterization
functions { 
matrix cov2cor(matrix C){
    int dm = rows(C);
    matrix[dm,dm] s;
    matrix[dm,dm] R;
    s = diag_matrix( 1 ./ sqrt(diagonal(C)) );
    R = s*C*s ; //quad_form_diag(C, s);
    return R;
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
  } else if (meanstructure == 1 || meanstructure == 2 ){
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
  //cov_matrix[ xC_marker >= 1 ? nt : 0 ] beta;
  row_vector[nt] beta0;
  vector[nt] beta1;
  
  // Separate into C_sd*C_R*C_sd
  // C_sd is defined in tp, as function of betas
  corr_matrix[nt] C_R;

  array[Q] matrix[nt, nt] A_raw;
  array[P] matrix[nt, nt] B_raw;

    // H1 init
  cov_matrix[nt] H1_init; 
  real< lower = 2 > nu; // nu for student_t

}
transformed parameters {
  array[T] cov_matrix[nt] H;
  array[T-1] matrix[nt,nt] rr;
  array[T] vector[nt] mu;

  matrix[nt, nt] A_part = diag_matrix( rep_vector(0.0, nt));
  matrix[nt, nt] B_part = diag_matrix( rep_vector(0.0, nt));

  matrix[nt+1, nt] beta = append_row( beta0, diag_matrix(beta1) );
  row_vector[nt] C_sd;
  cov_matrix[nt] Cnst; // Const is symmetric, A, B, are not  
    
  // Initialize model parameters
  mu[1,] = phi0;
  H[1,] = H1_init;

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

    // reset A_part and B_part to zero for each iteration t
    A_part = diag_matrix( rep_vector(0.0, nt));
    B_part = diag_matrix( rep_vector(0.0, nt));
        
    for (q in 1:min( t-1, Q) ) {
      rr[t-q,] = ( rts[t-q,] - mu[t-q,] )*( rts[t-q,] - mu[t-q,] )';
      A_part = A_part + A_raw[q]' * rr[t-q,] * A_raw[q];
    }
    for (p in 1:min( t-1, P) ) {
      B_part = B_part + B_raw[p]' * H[t-p,] * B_raw[p];
    }
    if( xC_marker == 0 ) {
      C_sd = exp( beta0 ); 
      Cnst =  quad_form_diag(C_R, C_sd );
      H[t,] = Cnst + A_part +  B_part;
    } else if( xC_marker >= 1) {
      C_sd = exp( append_col( 1.0, xC[t]' ) * beta ); 
      Cnst =  quad_form_diag(C_R, C_sd );
      H[t,] = Cnst  + A_part +  B_part;
    }
  }
}
model {
  // priors
  // https://mc-stan.org/documentation/case-studies/mle-params.html

  // Prior on nu for student_t
  nu ~ normal( nt, 50 );

  // Prior for initial state
  H1_init ~ wishart(nt + 1.0, diag_matrix(rep_vector(1.0, nt)) );

  to_vector(theta) ~ std_normal();
  to_vector(phi) ~ std_normal(); 
  //  to_vector(phi0) ~ normal();
  phi0 ~ multi_normal(rts_m, diag_matrix( rts_sd ) );

  to_vector(beta0) ~ std_normal();
  to_vector(beta1) ~ std_normal();
  C_R ~ lkj_corr( 1 );

  for(q in 1:Q) {
    to_vector(A_raw[q]) ~ std_normal();
  }
  for(p in 1:P) {
    to_vector(B_raw[p]) ~ std_normal();
  }
    
  // likelihood
  if ( distribution == 0 ) {
    for(t in 1:T){
      target += multi_normal_lpdf(rts[t, ]| mu[t,], H[t,]);
    }
  } else if ( distribution == 1 ) {
    for(t in 1:T){
      target += multi_student_t_lpdf( rts[t, ] | nu, mu[t,], H[t,]);
    }
  }
}
//
generated quantities {
  array[Q] matrix[nt, nt] A = A_raw;
  array[P] matrix[nt, nt] B = B_raw;
  matrix[nt,T] rts_out;
  array[T] real log_lik;
  corr_matrix[nt] corC;
  array[T] corr_matrix[nt] corH;
  row_vector[nt] C_var;

  for(q in 1:Q) {
    if(A[q,1,1] < 0) {
      A[q] = -A[q];
    }
  }
  for(p in 1:P) {
    if(B[p,1,1] < 0) {
      B[p] = -B[p];
    }
  }

//Const = multiply_lower_tri_self_transpose(Cnst);
  corC = C_R;
  C_var = exp(2*beta0 );

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
