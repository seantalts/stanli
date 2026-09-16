data{
    array[820] int H;
     vector[820] S;
}
parameters{
     real a;
     real bS;
}
model{
     vector[820] p;
    bS ~ normal( 0 , 0.5 );
    a ~ normal( 0 , 1 );
    for ( i in 1:820 ) {
        p[i] = a + bS * S[i];
        p[i] = inv_logit(p[i]);
    }
    H ~ binomial( 10 , p );
}

