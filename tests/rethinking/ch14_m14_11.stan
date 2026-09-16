functions{


    matrix cov_GPL1(matrix x, real sq_alpha, real sq_rho, real delta) {
        int N = dims(x)[1];
        matrix[N, N] K;
        for (i in 1:(N-1)) {
          K[i, i] = sq_alpha + delta;
          for (j in (i + 1):N) {
            K[i, j] = sq_alpha * exp(-sq_rho * x[i,j] );
            K[j, i] = K[i, j];
          }
        }
        K[N, N] = sq_alpha + delta;
        return K;
    }
}
data{
     matrix[151,151] R;
     matrix[151,151] V;
     matrix[151,151] Imat;
     int N_spp;
     vector[151] B;
     vector[151] G;
     vector[151] M;
     matrix[151,151] Dmat;
}
parameters{
     real a;
     real bG;
     real bM;
     real<lower=0> etasq;
     real<lower=0> rhosq;
}
model{
     vector[151] mu;
     matrix[N_spp,N_spp] SIGMA;
    rhosq ~ normal( 3 , 0.25 );
    etasq ~ normal( 1 , 0.25 );
    bM ~ normal( 0 , 0.5 );
    bG ~ normal( 0 , 0.5 );
    a ~ normal( 0 , 1 );
    SIGMA = cov_GPL1(Dmat, etasq, rhosq, 0.01);
    for ( i in 1:151 ) {
        mu[i] = a + bM * M[i] + bG * G[i];
    }
    B ~ multi_normal( mu , SIGMA );
}

