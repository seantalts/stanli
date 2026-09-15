data{
     int N;
    array[100] int RC;
    array[100] int notes;
    array[100] int cat;
}
parameters{
     real a;
     real b;
     real<lower=0,upper=1> k;
}
model{
     vector[100] lambda;
    k ~ beta( 2 , 2 );
    for ( i in 1:100 ) 
        if ( RC[i] == 0 ) cat[i] ~ bernoulli( k );
    b ~ normal( 0 , 0.5 );
    a ~ normal( 0 , 1 );
    for ( i in 1:100 ) 
        if ( RC[i] == 1 ) target += log_sum_exp(log(k) + poisson_lpmf(notes[i] | exp(a + b)), log(1 - k) + poisson_lpmf(notes[i] | exp(a)));
    for ( i in 1:100 ) {
        lambda[i] = a + b * cat[i];
        lambda[i] = exp(lambda[i]);
    }
    for ( i in 1:100 ) 
        if ( RC[i] == 0 ) notes[i] ~ poisson( lambda[i] );
}

