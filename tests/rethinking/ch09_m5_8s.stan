data{
     vector[100] height;
     vector[100] leg_right;
     vector[100] leg_left;
}
parameters{
     real a;
     real bl;
     real br;
     real<lower=0> sigma;
}
model{
     vector[100] mu;
    sigma ~ exponential( 1 );
    br ~ normal( 2 , 10 );
    bl ~ normal( 2 , 10 );
    a ~ normal( 10 , 100 );
    for ( i in 1:100 ) {
        mu[i] = a + bl * leg_left[i] + br * leg_right[i];
    }
    height ~ normal( mu , sigma );
}

