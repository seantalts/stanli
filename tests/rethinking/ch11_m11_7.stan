data{
    array[12] int applications;
    array[12] int admit;
    array[12] int gid;
}
parameters{
     vector[2] a;
}
model{
     vector[12] p;
    a ~ normal( 0 , 1.5 );
    for ( i in 1:12 ) {
        p[i] = a[gid[i]];
        p[i] = inv_logit(p[i]);
    }
    admit ~ binomial( applications , p );
}

