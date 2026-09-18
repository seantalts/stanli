data{
     vector[2] y;
}
parameters{
     real alpha;
     real<lower=0> sigma;
}
model{
     real mu;
    sigma ~ exponential( 1 );
    alpha ~ normal( 1 , 10 );
    mu = alpha;
    y ~ normal( mu , sigma );
}

