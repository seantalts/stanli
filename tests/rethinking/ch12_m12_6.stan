data{
    array[9930] int R;
    array[9930] int contact;
    array[9930] int intention;
    array[9930] int action;
    array[9930] int E;
     vector[7] alpha;
}
parameters{
     ordered[6] kappa;
     real bE;
     real bC;
     real bI;
     real bA;
     simplex[7] delta;
}
model{
     vector[9930] phi;
     vector[8] delta_j;
    delta ~ dirichlet( alpha );
    delta_j = append_row(0, delta);
    bA ~ normal( 0 , 1 );
    bI ~ normal( 0 , 1 );
    bC ~ normal( 0 , 1 );
    bE ~ normal( 0 , 1 );
    kappa ~ normal( 0 , 1.5 );
    for ( i in 1:9930 ) {
        phi[i] = bE * sum(delta_j[1:E[i]]) + bA * action[i] + bI * intention[i] + bC * contact[i];
    }
    for ( i in 1:9930 ) R[i] ~ ordered_logistic( phi[i] , kappa );
}

