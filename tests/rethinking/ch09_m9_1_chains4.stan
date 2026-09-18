data{
     vector[170] log_gdp_std;
     vector[170] rugged_std;
    array[170] int cid;
}
parameters{
     vector[2] a;
     vector[2] b;
     real<lower=0> sigma;
}
model{
     vector[170] mu;
    sigma ~ exponential( 1 );
    b ~ normal( 0 , 0.3 );
    a ~ normal( 1 , 0.1 );
    for ( i in 1:170 ) {
        mu[i] = a[cid[i]] + b[cid[i]] * (rugged_std[i] - 0.215);
    }
    log_gdp_std ~ normal( mu , sigma );
}

