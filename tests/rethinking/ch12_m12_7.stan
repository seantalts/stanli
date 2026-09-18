data{
    array[7] int alpha;
    array[9930] int E;
    array[9930] int R;
    array[9930] int contact;
    array[9930] int intention;
    array[9930] int action;
     vector[9930] edu_norm;
}
parameters{
     real bE;
     real bC;
     real bI;
     real bA;
     ordered[6] cutpoints;
}
model{
     vector[9930] mu;
    cutpoints ~ normal( 0 , 1.5 );
    bA ~ normal( 0 , 1 );
    bI ~ normal( 0 , 1 );
    bC ~ normal( 0 , 1 );
    bE ~ normal( 0 , 1 );
    for ( i in 1:9930 ) {
        mu[i] = bE * edu_norm[i] + bA * action[i] + bI * intention[i] + bC * contact[i];
    }
    for ( i in 1:9930 ) R[i] ~ ordered_logistic( mu[i] , cutpoints );
}

