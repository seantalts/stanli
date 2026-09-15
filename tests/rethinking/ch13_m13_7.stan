data{
     int N;
}
parameters{
     real v;
     real x;
}
model{
    x ~ normal( 0 , exp(v) );
    v ~ normal( 0 , 3 );
}

