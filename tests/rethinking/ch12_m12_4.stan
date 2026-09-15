data{
    array[9930] int R;
}
parameters{
     ordered[6] cutpoints;
}
model{
    cutpoints ~ normal( 0 , 1.5 );
    for ( i in 1:9930 ) R[i] ~ ordered_logistic( 0 , cutpoints );
}

