functions {
  int max_int(array[] int test) {
    int max_elem;
    if (size(test) == 0) reject("Test array must have length greater than 0.");
    max_elem = test[1];
    for (i in 2 : size(test)) max_elem = max(max_elem, test[i]);
    return max_elem;
  }
}
data {
  int M;
  array[M] int Nc;
  array[M] vector[max_int(Nc)] w;
}
parameters {
  real mu;
}
model {
  for (m in 1 : M) w[m] ~ normal(mu, 1);
}
