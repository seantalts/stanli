data {
  int<lower=1> G;
  int<lower=1> M;
  array[G] int<lower=1, upper=M> m;
  array[G] int<lower=1, upper=M> k;
  array[G] matrix[M, M] L;
}
parameters {
  vector[M] z;
}
model {
  for (g in 1:G)
    target += sum(L[g, :m[g], :m[g]] * z[:k[g]]);
  z ~ normal(0, 1);
}
