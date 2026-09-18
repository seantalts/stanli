data{
     matrix[151,151] V;
     matrix[151,151] Imat;
     int N_spp;
     vector[151] B;
     vector[151] G;
     vector[151] M;
     matrix[151,151] R;
}
parameters{
     real a;
     real bG;
     real bM;
     real<lower=0> sigma_sq;
}
model{
     vector[151] mu;
     matrix[N_spp,N_spp] SIGMA;
    sigma_sq ~ exponential( 1 );
    bM ~ normal( 0 , 0.5 );
    bG ~ normal( 0 , 0.5 );
    a ~ normal( 0 , 1 );
    SIGMA = R * sigma_sq;
    for ( i in 1:151 ) {
        mu[i] = a + bM * M[i] + bG * G[i];
    }
    B ~ multi_normal( mu , SIGMA );
}

