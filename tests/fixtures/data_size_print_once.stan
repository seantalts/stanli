functions {
  int sized(int n) {
    print("sizing ", n);
    return n;
  }
}
data {
  int N;
  array[sized(N)] real w;
}
parameters {
  real mu;
}
model {
  w ~ normal(mu, 1);
}
