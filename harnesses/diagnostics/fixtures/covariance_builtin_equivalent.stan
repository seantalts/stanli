
functions {
  matrix assemble(vector x, real amplitude, real rate, real jitter) {
    int N = rows(x);
    matrix[N,N] K;
    for (i in 1:N) {
      K[i,i] = amplitude + jitter;
      if (i < N) for (j in (i+1):N) {
        K[i,j] = amplitude * exp(-rate * square(x[i]-x[j]));
        K[j,i] = K[i,j];
      }
    }
    return K;
  }
  matrix factor(vector x, real amplitude, real rate, real jitter) {
    return cholesky_decompose(assemble(x, amplitude, rate, jitter));
  }
  array[] matrix make_array(vector x, real amplitude, real rate, real jitter) {
    int N = rows(x);
    array[2] matrix[N,N] A;
    A[1] = assemble(x, amplitude, rate, jitter);
    A[2] = A[1];
    return A;
  }
}

data {
  int<lower=0> N;
  vector[N] x;
  vector[N] y;
  real<lower=0> jitter;
  int<lower=0,upper=1> selector;
  array[N] int<lower=1,upper=max(1,N)> permutation;
}

parameters {
  real<lower=0> amplitude;
  real<lower=0> rate;
  vector[N] z;
}
transformed parameters { matrix[N,N] K;K = add_diag(gp_exp_quad_cov(to_array_1d(x), sqrt(amplitude), inv_sqrt(2*rate)), jitter);
      matrix[N,N] L = cholesky_decompose(K);
      vector[N] mu = L*z;
    } model { amplitude ~ exponential(1); rate ~ exponential(1); z ~ std_normal(); y ~ normal(mu,1); }
    generated quantities { real checksum = sum(L); }
    