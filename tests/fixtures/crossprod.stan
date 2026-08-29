data {
  matrix[2, 3] d;
}
transformed data {
  matrix[3, 3] data_gram = crossprod(d);
}
parameters {
  matrix[2, 3] a;
}
model {
  matrix[3, 3] gram = crossprod(a);
  to_vector(a) ~ normal(0, 1);
  to_vector(gram) ~ normal(to_vector(data_gram), 1);
}
generated quantities {
  matrix[3, 3] gq_gram = crossprod(a);
}
