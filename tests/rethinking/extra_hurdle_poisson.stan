data{
    array[7] int y;
}
parameters{
     real<lower=0,upper=1> p;
     real<lower=0> lambda;
}
model{
    lambda ~ exponential( 1 );
    p ~ beta( 2 , 2 );
    for ( i in 1:7 ) 
        if ( y[i] > 0 ) target += log1m(p) + poisson_lpmf(y[i] | lambda) - log1m_exp(-lambda);
    for ( i in 1:7 ) 
        if ( y[i] == 0 ) target += bernoulli_lpmf(1 | p);
}

