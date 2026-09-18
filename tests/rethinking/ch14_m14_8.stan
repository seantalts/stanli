functions{


    matrix cov_GPL2(matrix x, real sq_alpha, real sq_rho, real delta) {
        int N = dims(x)[1];
        matrix[N, N] K;
        for (i in 1:(N-1)) {
          K[i, i] = sq_alpha + delta;
          for (j in (i + 1):N) {
            K[i, j] = sq_alpha * exp(-sq_rho * square(x[i,j]) );
            K[j, i] = K[i, j];
          }
        }
        K[N, N] = sq_alpha + delta;
        return K;
    }
}
data{
    array[10] int T;
    array[10] int society;
    array[10] int P;
     matrix[10,10] Dmat;
}
parameters{
     vector[10] k;
     real<lower=0> g;
     real<lower=0> b;
     real<lower=0> a;
     real<lower=0> etasq;
     real<lower=0> rhosq;
}
model{
     vector[10] lambda;
     matrix[10,10] SIGMA;
    rhosq ~ exponential( 0.5 );
    etasq ~ exponential( 2 );
    a ~ exponential( 1 );
    b ~ exponential( 1 );
    g ~ exponential( 1 );
    SIGMA = cov_GPL2(Dmat, etasq, rhosq, 0.01);
    k ~ multi_normal( rep_vector(0,10) , SIGMA );
    for ( i in 1:10 ) {
        lambda[i] = (a * P[i]^b/g) * exp(k[society[i]]);
    }
    T ~ poisson( lambda );
}

