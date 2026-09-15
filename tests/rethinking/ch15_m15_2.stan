data{
     int N;
     vector[50] D_sd;
     vector[50] D_obs;
     vector[50] A;
     vector[50] M_sd;
     vector[50] M_obs;
}
parameters{
     vector[N] D_true;
     vector[N] M_true;
     real a;
     real bA;
     real bM;
     real<lower=0> sigma;
}
model{
     vector[50] mu;
    sigma ~ exponential( 1 );
    bM ~ normal( 0 , 0.5 );
    bA ~ normal( 0 , 0.5 );
    a ~ normal( 0 , 0.2 );
    M_true ~ normal( 0 , 1 );
    M_obs ~ normal( M_true , M_sd );
    for ( i in 1:50 ) {
        mu[i] = a + bA * A[i] + bM * M_true[i];
    }
    D_true ~ normal( mu , sigma );
    D_obs ~ normal( D_true , D_sd );
}

