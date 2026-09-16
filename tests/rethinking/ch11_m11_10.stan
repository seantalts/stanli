data{
    array[10] int T;
     vector[10] P;
    array[10] int cid;
}
parameters{
     vector[2] a;
     vector[2] b;
}
model{
     vector[10] lambda;
    b ~ normal( 0 , 0.2 );
    a ~ normal( 3 , 0.5 );
    for ( i in 1:10 ) {
        lambda[i] = a[cid[i]] + b[cid[i]] * P[i];
        lambda[i] = exp(lambda[i]);
    }
    T ~ poisson( lambda );
}
generated quantities{
    vector[10] log_lik;
     vector[10] lambda;
    for ( i in 1:10 ) {
        lambda[i] = a[cid[i]] + b[cid[i]] * P[i];
        lambda[i] = exp(lambda[i]);
    }
    for ( i in 1:10 ) log_lik[i] = poisson_lpmf( T[i] | lambda[i] );
}

