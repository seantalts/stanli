data{
     vector[100] y;
}
parameters{
     real a1;
     real a2;
     real<lower=0> sigma;
}
model{
     real mu;
    sigma ~ exponential( 1 );
    a2 ~ normal( 0 , 10 );
    a1 ~ normal( 0 , 10 );
    mu = a1 + a2;
    y ~ normal( mu , sigma );
}

