data{
    array[28] int cond;
    array[28] int side;
    array[28] int left_pulls;
    array[28] int treatment;
    array[28] int actor;
}
parameters{
     vector[7] a;
     vector[4] b;
}
model{
     vector[28] p;
    b ~ normal( 0 , 0.5 );
    a ~ normal( 0 , 1.5 );
    for ( i in 1:28 ) {
        p[i] = a[actor[i]] + b[treatment[i]];
        p[i] = inv_logit(p[i]);
    }
    left_pulls ~ binomial( 18 , p );
}
generated quantities{
    vector[28] log_lik;
     vector[28] p;
    for ( i in 1:28 ) {
        p[i] = a[actor[i]] + b[treatment[i]];
        p[i] = inv_logit(p[i]);
    }
    for ( i in 1:28 ) log_lik[i] = binomial_lpmf( left_pulls[i] | 18 , p[i] );
}

