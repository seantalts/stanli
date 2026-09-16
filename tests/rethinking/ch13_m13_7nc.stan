data{
     int N;
}
parameters{
     real v;
     real z;
}
model{
    z ~ normal( 0 , 1 );
    v ~ normal( 0 , 3 );
}
generated quantities{
     real x;
    x = z * exp(v);
}

