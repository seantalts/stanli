data{
    array[9930] int R;
    array[9930] int I;
    array[9930] int C;
    array[9930] int A;
}
parameters{
     real bIC;
     real bIA;
     real bC;
     real bI;
     real bA;
     ordered[6] cutpoints;
}
model{
     vector[9930] phi;
     vector[9930] BI;
    cutpoints ~ normal( 0 , 1.5 );
    bA ~ normal( 0 , 0.5 );
    bI ~ normal( 0 , 0.5 );
    bC ~ normal( 0 , 0.5 );
    bIA ~ normal( 0 , 0.5 );
    bIC ~ normal( 0 , 0.5 );
    for ( i in 1:9930 ) {
        BI[i] = bI + bIA * A[i] + bIC * C[i];
    }
    for ( i in 1:9930 ) {
        phi[i] = bA * A[i] + bC * C[i] + BI[i] * I[i];
    }
    for ( i in 1:9930 ) R[i] ~ ordered_logistic( phi[i] , cutpoints );
}

