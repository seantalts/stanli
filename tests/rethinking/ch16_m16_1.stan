data{
    array[544] int male;
     vector[544] age;
     vector[544] weight;
     vector[544] height;
     vector[544] w;
     vector[544] h;
}
parameters{
     real<lower=0,upper=1> p;
     real<lower=0> k;
     real<lower=0> sigma;
}
model{
     vector[544] mu;
    sigma ~ exponential( 1 );
    k ~ exponential( 0.5 );
    p ~ beta( 2 , 18 );
    for ( i in 1:544 ) {
        mu[i] = 3.141593 * k * p^2 * h[i]^3;
        mu[i] = log(mu[i]);
    }
    w ~ lognormal( mu , sigma );
}

