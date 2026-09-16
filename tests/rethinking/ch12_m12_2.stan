data{
    array[10] int T;
    array[10] int P;
    array[10] int cid;
}
parameters{
     vector[2] a;
     vector<lower=0>[2] b;
     real<lower=0> g;
     real<lower=0> phi;
}
model{
     vector[10] lambda;
    phi ~ exponential( 1 );
    g ~ exponential( 1 );
    b ~ exponential( 1 );
    a ~ normal( 1 , 1 );
    for ( i in 1:10 ) {
        lambda[i] = exp(a[cid[i]]) * P[i]^b[cid[i]]/g;
    }
    T ~ neg_binomial_2( lambda , phi );
}
generated quantities{
    vector[10] log_lik;
     vector[10] lambda;
    for ( i in 1:10 ) {
        lambda[i] = exp(a[cid[i]]) * P[i]^b[cid[i]]/g;
    }
    for ( i in 1:10 ) log_lik[i] = neg_binomial_2_lpmf( T[i] | lambda[i] , phi );
}

