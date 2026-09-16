data{
    array[48] int N;
    array[48] int S;
    array[48] int tank;
}
parameters{
     vector[48] a;
     real a_bar;
     real<lower=0> sigma;
}
model{
     vector[48] p;
    sigma ~ exponential( 1 );
    a_bar ~ normal( 0 , 1.5 );
    a ~ normal( a_bar , sigma );
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

