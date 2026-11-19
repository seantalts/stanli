data {
  int<lower=1> G;
  int<lower=1> M;
  array[G] int<lower=1, upper=M> m;
  int<lower=1> N;
  vector[N] y;
  array[G] matrix[M, M] L;
}
parameters {
  vector[N] z;
  real<lower=0> sigma;
  real<lower=0> tau;
}
model {
  array[G] matrix[M, M] scaled;
  for (g in 1:G) scaled[g] = tau * L[g];
  int pos = 0;
  for (g in 1:G) {
    array[m[g]] int idx = linspaced_int_array(m[g], pos + 1, pos + m[g]);
    vector[m[g]] v = z[idx];
    target += normal_lpdf(L[g, :m[g], :m[g]] * v | mean(y[idx]), sigma);
    target += normal_lpdf(scaled[g, :m[g], :m[g]] * v | 0, 1);
    pos += m[g];
  }
  z ~ normal(0, 1);
  sigma ~ lognormal(0, 1);
  tau ~ lognormal(0, 1);
}
generated quantities {
  real fit = 0;
  {
    int pos = 0;
    for (g in 1:G) {
      int n = m[g];
      array[n] int idx = linspaced_int_array(n, pos + 1, pos + n);
      fit += sum(L[g, :n, :n] * z[idx]);
      pos += n;
    }
  }
}
