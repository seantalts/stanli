data{
     vector[17] K;
     vector[17] M;
     vector[17] B;
}
parameters{
     real nu;
     real a;
     real bM;
     real bB;
     real<lower=0> sigma_B;
     real<lower=0> sigma;
}
model{
     vector[17] mu;
    sigma ~ exponential( 1 );
    sigma_B ~ exponential( 1 );
    bB ~ normal( 0 , 0.5 );
    bM ~ normal( 0 , 0.5 );
    a ~ normal( 0 , 0.5 );
    nu ~ normal( 0 , 0.5 );
    B ~ normal( nu , sigma_B );
    for ( i in 1:17 ) {
        mu[i] = a + bB * B[i] + bM * M[i];
    }
    K ~ normal( mu , sigma );
}

