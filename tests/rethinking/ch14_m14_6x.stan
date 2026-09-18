data{
     vector[500] W;
     vector[500] E;
     vector[500] Q;
}
parameters{
     real aE;
     real aW;
     real bQE;
     real bEW;
     corr_matrix[2] Rho;
     vector<lower=0>[2] Sigma;
}
model{
     vector[500] muW;
     vector[500] muE;
    Sigma ~ exponential( 1 );
    Rho ~ lkj_corr( 2 );
    bEW ~ normal( 0 , 0.5 );
    bQE ~ normal( 0 , 0.5 );
    aW ~ normal( 0 , 0.2 );
    aE ~ normal( 0 , 0.2 );
    for ( i in 1:500 ) {
        muE[i] = aE + bQE * Q[i];
    }
    for ( i in 1:500 ) {
        muW[i] = aW + bEW * E[i];
    }
    {
    array[500] vector[2] YY;
    array[500] vector[2] MU;
    for ( j in 1:500 ) MU[j] = [ muW[j] , muE[j] ]';
    for ( j in 1:500 ) YY[j] = [ W[j] , E[j] ]';
    YY ~ multi_normal( MU , quad_form_diag(Rho , Sigma) );
    }
}

