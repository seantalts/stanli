data{
    array[12] int admit;
    array[12] int rej;
}
parameters{
     real a2;
     real a1;
}
model{
     real lambda1;
     real lambda2;
    a1 ~ normal( 0 , 1.5 );
    a2 ~ normal( 0 , 1.5 );
    lambda2 = a2;
    lambda2 = exp(lambda2);
    lambda1 = a1;
    lambda1 = exp(lambda1);
    rej ~ poisson( lambda2 );
    admit ~ poisson( lambda1 );
}

