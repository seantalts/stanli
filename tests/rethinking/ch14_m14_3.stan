data{
    array[504] int L;
    array[504] int block_id;
    array[504] int actor;
    array[504] int tid;
}
parameters{
     matrix[4,7] z_actor;
     matrix[4,6] z_block;
     vector[4] g;
     vector<lower=0>[4] sigma_actor;
     cholesky_factor_corr[4] L_Rho_actor;
     vector<lower=0>[4] sigma_block;
     cholesky_factor_corr[4] L_Rho_block;
}
transformed parameters{
     matrix[7,4] alpha;
     matrix[6,4] beta;
    beta = (diag_pre_multiply(sigma_block, L_Rho_block) * z_block)';
    alpha = (diag_pre_multiply(sigma_actor, L_Rho_actor) * z_actor)';
}
model{
     vector[504] p;
    L_Rho_block ~ lkj_corr_cholesky( 2 );
    sigma_block ~ exponential( 1 );
    L_Rho_actor ~ lkj_corr_cholesky( 2 );
    sigma_actor ~ exponential( 1 );
    g ~ normal( 0 , 1 );
    to_vector( z_block ) ~ normal( 0 , 1 );
    to_vector( z_actor ) ~ normal( 0 , 1 );
    for ( i in 1:504 ) {
        p[i] = g[tid[i]] + alpha[actor[i], tid[i]] + beta[block_id[i], tid[i]];
        p[i] = inv_logit(p[i]);
    }
    L ~ binomial( 1 , p );
}
generated quantities{
    vector[504] log_lik;
     vector[504] p;
     matrix[4,4] Rho_actor;
     matrix[4,4] Rho_block;
    Rho_block = multiply_lower_tri_self_transpose(L_Rho_block);
    Rho_actor = multiply_lower_tri_self_transpose(L_Rho_actor);
    for ( i in 1:504 ) {
        p[i] = g[tid[i]] + alpha[actor[i], tid[i]] + beta[block_id[i], tid[i]];
        p[i] = inv_logit(p[i]);
    }
    for ( i in 1:504 ) log_lik[i] = binomial_lpmf( L[i] | 1 , p[i] );
}

