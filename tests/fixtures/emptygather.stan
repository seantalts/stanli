// Data-dependent gather width (issue #133): Jev[1:Nev] gathers a slice
// whose length is computed in transformed data, so whether the gather is
// empty depends on the data, not the program. Nev == 0 must compile and
// contribute exactly zero, matching CmdStan. One fixture, two datasets.
functions {
  real g_lpdf(vector y, vector m) {
    return dot_product(y, exp(m)) + sum(m);
  }
}
data {
  int<lower=1> N;
  vector[N] Y;
  array[N] int cens;
}
transformed data {
  int Nev = 0;
  array[N] int Jev;
  for (n in 1:N) {
    if (cens[n] == 0) {
      Nev += 1;
      Jev[Nev] = n;
    }
  }
}
parameters {
  vector[N] mu;
}
model {
  vector[N] m2 = mu * 2.0;
  target += g_lpdf(Y[Jev[1:Nev]] | m2[Jev[1:Nev]]);
  target += normal_lpdf(mu | 0, 1);
}
