data{
    array[10] int T;
    array[10] int P;
    array[10] int cid;
}
parameters{
     vector[2] a;
     vector<lower=0>[2] b;
     real<lower=0> g;
}
model{
     vector[10] lambda;
    g ~ exponential( 1 );
    b ~ exponential( 1 );
    a ~ normal( 1 , 1 );
    for ( i in 1:10 ) {
        lambda[i] = exp(a[cid[i]]) * P[i]^b[cid[i]]/g;
    }
    T ~ poisson( lambda );
}
generated quantities{
    vector[10] log_lik;
     vector[10] lambda;
    for ( i in 1:10 ) {
        lambda[i] = exp(a[cid[i]]) * P[i]^b[cid[i]]/g;
    }
    for ( i in 1:10 ) log_lik[i] = poisson_lpmf( T[i] | lambda[i] );
}

