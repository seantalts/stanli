data{
    array[12] int applications;
    array[12] int admit;
    array[12] int dept_id;
    array[12] int gid;
}
parameters{
     vector[2] a;
     vector[6] delta;
}
model{
     vector[12] p;
    delta ~ normal( 0 , 1.5 );
    a ~ normal( 0 , 1.5 );
    for ( i in 1:12 ) {
        p[i] = a[gid[i]] + delta[dept_id[i]];
        p[i] = inv_logit(p[i]);
    }
    admit ~ binomial( applications , p );
}

