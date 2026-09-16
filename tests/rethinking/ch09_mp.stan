data{
     int y;
}
parameters{
     real a;
     real b;
}
model{
    b ~ cauchy( 0 , 1 );
    a ~ normal( 0 , 1 );
}

