data{
     int N;
     vector[50] D_sd;
     vector[50] D_obs;
     vector[50] M;
     vector[50] A;
}
parameters{
     vector[N] D_true;
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
    for ( i in 1:50 ) {
        mu[i] = a + bA * A[i] + bM * M[i];
    }
    D_true ~ normal( mu , sigma );
    D_obs ~ normal( D_true , D_sd );
}

