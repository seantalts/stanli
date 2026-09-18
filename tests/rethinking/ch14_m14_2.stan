data{
    array[504] int L;
    array[504] int block_id;
    array[504] int actor;
    array[504] int tid;
}
parameters{
    array[7] vector[4] alpha;
    array[6] vector[4] beta;
     vector[4] g;
     vector<lower=0>[4] sigma_actor;
     corr_matrix[4] Rho_actor;
     vector<lower=0>[4] sigma_block;
     corr_matrix[4] Rho_block;
}
model{
     vector[504] p;
    Rho_block ~ lkj_corr( 4 );
    sigma_block ~ exponential( 1 );
    Rho_actor ~ lkj_corr( 4 );
    sigma_actor ~ exponential( 1 );
    g ~ normal( 0 , 1 );
    beta ~ multi_normal( rep_vector(0,4) , quad_form_diag(Rho_block , sigma_block) );
    alpha ~ multi_normal( rep_vector(0,4) , quad_form_diag(Rho_actor , sigma_actor) );
    for ( i in 1:504 ) {
        p[i] = g[tid[i]] + alpha[actor[i], tid[i]] + beta[block_id[i], tid[i]];
        p[i] = inv_logit(p[i]);
    }
    L ~ binomial( 1 , p );
}

