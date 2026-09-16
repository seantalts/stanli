data{
     vector[500] W;
     vector[500] Q;
     vector[500] E;
}
parameters{
     real aW;
     real bEW;
     real bQW;
     real<lower=0> sigma;
}
model{
     vector[500] mu;
    sigma ~ exponential( 1 );
    bQW ~ normal( 0 , 0.5 );
    bEW ~ normal( 0 , 0.5 );
    aW ~ normal( 0 , 0.2 );
    for ( i in 1:500 ) {
        mu[i] = aW + bEW * E[i] + bQW * Q[i];
    }
    W ~ normal( mu , sigma );
}

