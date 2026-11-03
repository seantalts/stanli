functions {
  real public(real class) {
    return 2 * class;
  }
}
data {
  int new;
  vector[new] class;
}
parameters {
  real explicit;
}
transformed parameters {
  real signed = public(explicit);
}
model {
  real total = 0;
  for (volatile in 1:new) total += class[volatile];
  explicit ~ normal(total, 1);
}
generated quantities {
  real friend = signed + sum(class);
}
