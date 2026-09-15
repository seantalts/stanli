data{
    array[504] int pulled_left;
    array[504] int treatment;
    array[504] int block_id;
    array[504] int actor;
}
parameters{
     vector[4] b;
     vector[7] a;
     vector[6] g;
     real a_bar;
     real<lower=0> sigma_a;
     real<lower=0> sigma_g;
}
model{
     vector[504] p;
    sigma_g ~ exponential( 1 );
    sigma_a ~ exponential( 1 );
    a_bar ~ normal( 0 , 1.5 );
    g ~ normal( 0 , sigma_g );
    a ~ normal( a_bar , sigma_a );
    b ~ normal( 0 , 0.5 );
    for ( i in 1:504 ) {
        p[i] = a[actor[i]] + g[block_id[i]] + b[treatment[i]];
        p[i] = inv_logit(p[i]);
    }
    pulled_left ~ binomial( 1 , p );
}
generated quantities{
    vector[504] log_lik;
     vector[504] p;
    for ( i in 1:504 ) {
        p[i] = a[actor[i]] + g[block_id[i]] + b[treatment[i]];
        p[i] = inv_logit(p[i]);
    }
    for ( i in 1:504 ) log_lik[i] = binomial_lpmf( pulled_left[i] | 1 , p[i] );
}

