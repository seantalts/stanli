data{
    array[60] int Ni;
    array[60] int Si;
    array[60] int pond;
}
parameters{
     vector[60] a_pond;
     real a_bar;
     real<lower=0> sigma;
}
model{
     vector[60] p;
    sigma ~ exponential( 1 );
    a_bar ~ normal( 0 , 1.5 );
    a_pond ~ normal( a_bar , sigma );
    for ( i in 1:60 ) {
        p[i] = a_pond[pond[i]];
        p[i] = inv_logit(p[i]);
    }
    Si ~ binomial( Ni , p );
}

