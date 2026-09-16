data{
    array[504] int pulled_left;
    array[504] int treatment;
    array[504] int block_id;
    array[504] int actor;
}
parameters{
     vector[4] b;
     vector[7] z;
     vector[6] x;
     real a_bar;
     real<lower=0> sigma_a;
     real<lower=0> sigma_g;
}
model{
     vector[504] p;
    sigma_g ~ exponential( 1 );
    sigma_a ~ exponential( 1 );
    a_bar ~ normal( 0 , 1.5 );
    x ~ normal( 0 , 1 );
    z ~ normal( 0 , 1 );
    b ~ normal( 0 , 0.5 );
    for ( i in 1:504 ) {
        p[i] = a_bar + z[actor[i]] * sigma_a + x[block_id[i]] * sigma_g + b[treatment[i]];
        p[i] = inv_logit(p[i]);
    }
    pulled_left ~ binomial( 1 , p );
}
generated quantities{
     vector[7] a;
     vector[6] g;
    g = x * sigma_g;
    a = a_bar + z * sigma_a;
}

