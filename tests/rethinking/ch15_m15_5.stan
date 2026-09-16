functions{


    vector merge_missing( array[] int miss_indexes , vector x_obs , vector x_miss ) {
        int N = dims(x_obs)[1];
        int N_miss = dims(x_miss)[1];
        vector[N] merged;
        merged = x_obs;
        for ( i in 1:N_miss )
            merged[ miss_indexes[i] ] = x_miss[i];
        return merged;
    }
}
data{
     vector[29] K;
     vector[29] M;
     vector[29] B;
    array[12] int B_missidx;
}
parameters{
     real nu;
     real a;
     real bM;
     real bB;
     real<lower=0> sigma_B;
     real<lower=0> sigma;
     vector[12] B_impute;
}
model{
     vector[29] mu;
     vector[29] B_merge;
    sigma ~ exponential( 1 );
    sigma_B ~ exponential( 1 );
    bB ~ normal( 0 , 0.5 );
    bM ~ normal( 0 , 0.5 );
    a ~ normal( 0 , 0.5 );
    nu ~ normal( 0 , 0.5 );
    B_merge = merge_missing(B_missidx, to_vector(B), B_impute);
    B_merge ~ normal( nu , sigma_B );
    for ( i in 1:29 ) {
        mu[i] = a + bB * B_merge[i] + bM * M[i];
    }
    K ~ normal( mu , sigma );
}

