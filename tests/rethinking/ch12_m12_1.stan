data{
    array[12] int N;
    array[12] int A;
    array[12] int gid;
}
parameters{
     vector[2] a;
     real<lower=0> phi;
}
transformed parameters{
     real theta;
    theta = phi + 2;
}
model{
     vector[12] pbar;
    phi ~ exponential( 1 );
    a ~ normal( 0 , 1.5 );
    for ( i in 1:12 ) {
        pbar[i] = a[gid[i]];
        pbar[i] = inv_logit(pbar[i]);
    }
    A ~ beta_binomial( N , pbar*theta , (1-pbar)*theta );
}

