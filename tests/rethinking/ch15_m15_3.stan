data{
    array[1000] int H;
     vector[1000] S;
}
parameters{
     real a;
     real bS;
}
model{
     vector[1000] p;
    bS ~ normal( 0 , 0.5 );
    a ~ normal( 0 , 1 );
    for ( i in 1:1000 ) {
        p[i] = a + bS * S[i];
        p[i] = inv_logit(p[i]);
    }
    H ~ binomial( 10 , p );
}

