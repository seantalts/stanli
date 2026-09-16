data{
    array[10] int cid;
     vector[10] P;
    array[10] int T;
}
parameters{
     real a;
}
model{
     real lambda;
    a ~ normal( 3 , 0.5 );
    lambda = a;
    lambda = exp(lambda);
    T ~ poisson( lambda );
}
generated quantities{
    vector[10] log_lik;
     real lambda;
    lambda = a;
    lambda = exp(lambda);
    for ( i in 1:10 ) log_lik[i] = poisson_lpmf( T[i] | lambda );
}

