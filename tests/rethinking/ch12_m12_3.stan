data{
    array[365] int y;
}
parameters{
     real ap;
     real al;
}
model{
     real p;
     real lambda;
    al ~ normal( 1 , 0.5 );
    ap ~ normal( -1.5 , 1 );
    lambda = al;
    lambda = exp(lambda);
    p = ap;
    p = inv_logit(p);
    for ( i in 1:365 ) {
        if ( y[i]==0 )
            target += log_mix( p , 0 , poisson_lpmf(0|lambda) );
        if ( y[i] > 0 )
            target += log1m( p ) + poisson_lpmf(y[i] | lambda );
    }
}

