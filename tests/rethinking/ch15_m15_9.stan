data{
     int N;
    array[100] int RC;
    array[100] int cat;
    array[100] int notes;
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
    for ( i in 1:100 ) {
        lambda[i] = a + b * cat[i];
        lambda[i] = exp(lambda[i]);
    }
    for ( i in 1:100 ) 
        if ( RC[i] == 1 ) target += log_sum_exp(log(k) + poisson_lpmf(notes[i] | exp(a + b)), log(1 - k) + poisson_lpmf(notes[i] | exp(a)));
    for ( i in 1:100 ) 
        if ( RC[i] == 0 ) notes[i] ~ poisson( lambda[i] );
}
generated quantities{
     vector[N] PrC1;
     vector[N] lpC1;
     vector[N] lpC0;
    for ( i in 1:N ) {
        lpC0[i] = log(1 - k) + poisson_lpmf(notes[i] | exp(a));
    }
    for ( i in 1:N ) {
        lpC1[i] = log(k) + poisson_lpmf(notes[i] | exp(a + b));
    }
    for ( i in 1:N ) {
        PrC1[i] = exp(lpC1[i])/(exp(lpC1[i]) + exp(lpC0[i]));
    }
}

