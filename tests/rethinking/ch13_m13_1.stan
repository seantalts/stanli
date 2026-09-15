data{
    array[48] int N;
    array[48] int S;
    array[48] int tank;
}
parameters{
     vector[48] a;
}
model{
     vector[48] p;
    a ~ normal( 0 , 1.5 );
    for ( i in 1:48 ) {
        p[i] = a[tank[i]];
        p[i] = inv_logit(p[i]);
    }
    S ~ binomial( N , p );
}
generated quantities{
    vector[48] log_lik;
     vector[48] p;
    for ( i in 1:48 ) {
        p[i] = a[tank[i]];
        p[i] = inv_logit(p[i]);
    }
    for ( i in 1:48 ) log_lik[i] = binomial_lpmf( S[i] | N[i] , p[i] );
}

