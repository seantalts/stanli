functions {
  int sized(int n) {
    print("sizing ", n);
    return n;
  }
}
data {
  int N;
  array[sized(N)] real w;
  array[sized(N)] real v;
}
transformed data {
  int k = sized(N);
}
parameters {
  real mu;
}
model {
  w ~ normal(mu, 1);
  v ~ normal(mu, k);
}
