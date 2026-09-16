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
     real muM;
     real muB;
     real a;
     real bM;
     real bB;
     real<lower=0> sigma;
     corr_matrix[2] Rho_BM;
     vector<lower=0>[2] Sigma_BM;
     vector[12] B_impute;
}
model{
     vector[29] mu;
     matrix[29,2] MB;
     vector[29] B_merge;
    Sigma_BM ~ exponential( 1 );
    Rho_BM ~ lkj_corr( 2 );
    sigma ~ exponential( 1 );
    bB ~ normal( 0 , 0.5 );
    bM ~ normal( 0 , 0.5 );
    a ~ normal( 0 , 0.5 );
    muB ~ normal( 0 , 0.5 );
    muM ~ normal( 0 , 0.5 );
    B_merge = merge_missing(B_missidx, to_vector(B), B_impute);
    MB = append_col(M, B_merge);
    {
    vector[2] MU;
    MU = [ muM , muB ]';
    for ( i in 1:29 ) MB[i,:] ~ multi_normal( MU , quad_form_diag(Rho_BM , Sigma_BM) );
    }
    for ( i in 1:29 ) {
        mu[i] = a + bB * B_merge[i] + bM * M[i];
    }
    K ~ normal( mu , sigma );
}

