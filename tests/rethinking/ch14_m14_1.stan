data{
     vector[200] wait;
    array[200] int afternoon;
    array[200] int cafe;
}
parameters{
     vector[20] b_cafe;
     vector[20] a_cafe;
     real a;
     real b;
     vector<lower=0>[2] sigma_cafe;
     real<lower=0> sigma;
     corr_matrix[2] Rho;
}
model{
     vector[200] mu;
    Rho ~ lkj_corr( 2 );
    sigma ~ exponential( 1 );
    sigma_cafe ~ exponential( 1 );
    b ~ normal( -1 , 0.5 );
    a ~ normal( 5 , 2 );
    {
    array[20] vector[2] YY;
    vector[2] MU;
    MU = [ a , b ]';
    for ( j in 1:20 ) YY[j] = [ a_cafe[j] , b_cafe[j] ]';
    YY ~ multi_normal( MU , quad_form_diag(Rho , sigma_cafe) );
    }
    for ( i in 1:200 ) {
        mu[i] = a_cafe[cafe[i]] + b_cafe[cafe[i]] * afternoon[i];
    }
    wait ~ normal( mu , sigma );
}

