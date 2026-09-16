data{
     vector[2] y;
}
parameters{
     real alpha;
     real<lower=0> sigma;
}
model{
     real mu;
    sigma ~ exponential( 1e-04 );
    alpha ~ normal( 0 , 1000 );
    mu = alpha;
    y ~ normal( mu , sigma );
}

