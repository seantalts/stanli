data{
    array[504] int pulled_left;
    array[504] int cond;
    array[504] int side;
    array[504] int actor;
}
parameters{
     vector[7] a;
     vector[2] bs;
     vector[2] bc;
}
model{
     vector[504] p;
    bc ~ normal( 0 , 0.5 );
    bs ~ normal( 0 , 0.5 );
    a ~ normal( 0 , 1.5 );
    for ( i in 1:504 ) {
        p[i] = a[actor[i]] + bs[side[i]] + bc[cond[i]];
        p[i] = inv_logit(p[i]);
    }
    pulled_left ~ binomial( 1 , p );
}
generated quantities{
    vector[504] log_lik;
     vector[504] p;
    for ( i in 1:504 ) {
        p[i] = a[actor[i]] + bs[side[i]] + bc[cond[i]];
        p[i] = inv_logit(p[i]);
    }
    for ( i in 1:504 ) log_lik[i] = binomial_lpmf( pulled_left[i] | 1 , p[i] );
}

